#pragma once
#include "Commands.h"

// Boiler plate code for adding a shortcut, mainly to avoid writing the same code multiple time
// Only configurable at compile time, so it should change in the future.

template <typename... Args> inline bool KeyPressed(ImGuiKey key, Args... others) {
    return KeyPressed(key) && KeyPressed(others...);
}
template <> inline bool KeyPressed(ImGuiKey key) { return ImGui::IsKeyDown(key); }

template <typename CommandT, ImGuiKey... Keys, typename... Args> inline void AddShortcut(Args &&...args) {
    static bool KeyPressedOnce = false;
    if (KeyPressed(Keys...)) {
        if (!KeyPressedOnce) {
            ExecuteAfterDraw<CommandT>(args...);
            KeyPressedOnce = true;
        }
    } else {
        KeyPressedOnce = false;
    }
}
