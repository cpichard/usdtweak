#include "MouseCapture.h"
#include <GLFW/glfw3.h>
#include "3rdparty/imgui/imgui.h"

void MouseCapture::Set(bool captured) {
    if (!_enabled) return;
    if (_captured == captured) return;
    _captured = captured;
    GLFWwindow *window = glfwGetCurrentContext();
    if (!window) return;
    ImGuiIO &io = ImGui::GetIO();
    if (captured) {
        _justCaptured = true;
        glfwGetCursorPos(window, &_savedX, &_savedY);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported())
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        io.MouseDelta = {0, 0};
    } else {
        _justCaptured = false;
        if (glfwRawMouseMotionSupported())
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        glfwSetCursorPos(window, _savedX, _savedY);
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }
}

void MouseCapture::ProcessFrame() {
    if (!_captured) return;
    ImGuiIO &io = ImGui::GetIO();
    if (_justCaptured && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        io.MouseDelta = {0, 0};
        _justCaptured = false;
    }
}
