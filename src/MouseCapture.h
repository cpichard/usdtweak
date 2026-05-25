#pragma once

// Manages GLFW cursor-disabled mode and absorbs the cursor-mode-transition
// delta discontinuity described in patches/glfw-3.4/.
class MouseCapture {
public:
    void Set(bool captured);
    bool Get() const { return _captured; }

    bool IsEnabled() const { return _enabled; }
    bool &Enabled() { return _enabled; }

    // Call once per frame (before consuming io.MouseDelta) to zero the
    // first-frame spike produced by GLFW silently resetting virtualCursorPos
    // on re-entry to GLFW_CURSOR_DISABLED without firing the cursorPos callback.
    void ProcessFrame();

private:
    bool _captured = false;
    bool _justCaptured = false;
    bool _enabled = true;
    double _savedX = 0.0;
    double _savedY = 0.0;
};
