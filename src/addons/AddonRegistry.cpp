#include "AddonRegistry.h"

#include <pxr/base/tf/registryManager.h>

PXR_NAMESPACE_USING_DIRECTIVE

UsdTweakAddonRegistry &UsdTweakAddonRegistry::GetInstance() {
    static UsdTweakAddonRegistry instance;
    return instance;
}

void UsdTweakAddonRegistry::Add(UsdTweakAddon addon) { _addons.push_back(std::move(addon)); }

void UsdTweakAddonRegistry::SubscribeAll() {
    TfRegistryManager::GetInstance().SubscribeTo<UsdTweakAddonRegistry>();
}
