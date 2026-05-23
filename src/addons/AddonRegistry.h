#pragma once

#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

///
/// AddonRegistry is the central list of addons known to usdtweak.
///
/// Each addon registers itself via TF_REGISTRY_FUNCTION_WITH_TAG in its own
/// translation unit; the Editor triggers registration once at startup by calling
/// TfRegistryManager::GetInstance().SubscribeTo<UsdTweakAddonRegistry>().
///
/// An addon is either:
///   - Kind::Window — a dockable ImGui window. The Editor wraps `draw` in
///     ImGui::Begin/End and persists the open/close state under the addon id.
///   - Kind::Action — a one-shot menu item. Clicking it runs `activate`
///     (typically to trigger a modal dialog).
///

struct UsdTweakAddon {
    enum class Kind { Window, Action };

    std::string id;                       // unique, stable; used for settings key
    std::string menuLabel;                // shown under the Addons menu
    Kind kind = Kind::Window;

    std::function<void()> draw;           // Kind::Window: drawn inside ImGui::Begin/End
    std::function<void()> activate;       // Kind::Action: fired from the menu

    std::function<bool()> isAvailable;    // optional; if returns false the menu item is greyed
    bool defaultOpen = false;             // Kind::Window only
    ImGuiWindowFlags windowFlags = 0;     // Kind::Window only
};

class UsdTweakAddonRegistry {
  public:
    static UsdTweakAddonRegistry &GetInstance();

    void Add(UsdTweakAddon addon);
    const std::vector<UsdTweakAddon> &GetAll() const { return _addons; }

    /// Trigger all pending TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, …)
    /// bodies. Must be called once, from the Editor constructor.
    void SubscribeAll();

  private:
    UsdTweakAddonRegistry() = default;
    std::vector<UsdTweakAddon> _addons;
};
