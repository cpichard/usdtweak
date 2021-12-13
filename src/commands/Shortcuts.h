#pragma once
#include "Commands.h"

// Boiler plate code for adding a shortcut, mainly to avoid writing the same code multiple time
// Only configurable at compile time, so it should change in the future.

template <typename T, typename... Args> inline bool KeyPressed(T key, Args... others) {
    return KeyPressed(key) && KeyPressed(others...);
}
template <> inline bool KeyPressed(size_t key) { return ImGui::GetIO().KeysDown[key]; }

template <typename CommandT, size_t... Keys, typename... Args> inline void AddShortcut(Args &&...args) {
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
