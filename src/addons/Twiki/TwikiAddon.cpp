#include "addons/Api.h"

#include "AgentChatPanel.h"

#include <pxr/base/tf/weakPtr.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usd/stage.h>

#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

constexpr const char *kAddonId = "Twiki";

// One panel per process, lazy-created on first draw. The destructor blocks
// on any in-flight worker, so we never destroy mid-frame — process exit is
// fine.
UsdAgent::AgentChatPanel &GetPanel() {
    static std::unique_ptr<UsdAgent::AgentChatPanel> panel = [] {
        auto stageFn = []() -> UsdStageRefPtr {
            return usdtweak::GetCurrentStage();
        };
        auto editLayerFn = []() -> SdfLayerRefPtr {
            const UsdEditTarget et = usdtweak::GetCurrentEditTarget();
            return TfCreateRefPtrFromProtectedWeakPtr(et.GetLayer());
        };
        auto selectionFn = []() -> Selection * {
            // Selection is mutated through usdtweak::Set/Add{Stage,Layer}PathSelection
            // (which fire UsdTweakSelectionChangedNotice). The agent's
            // select_prims tool still needs a non-const pointer for its
            // queued mutations — that path bypasses the API helpers today.
            // TODO: extend addons/Api.h with mutable-selection access so the
            // const_cast goes away.
            return const_cast<Selection *>(&usdtweak::GetSelection());
        };
        auto openFileFn = [](const std::string& path, bool asStage) {
            if (asStage) usdtweak::OpenStage(path);
            else         usdtweak::FindOrOpenLayer(path);
        };
        return std::make_unique<UsdAgent::AgentChatPanel>(
            std::move(stageFn), std::move(editLayerFn),
            std::move(selectionFn), std::move(openFileFn));
    }();
    return *panel;
}

void DrawTwiki() {
    GetPanel().Draw();
}

} // namespace

TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, Twiki) {
    UsdTweakAddon addon;
    addon.id = kAddonId;
    addon.menuLabel = "Twiki";
    addon.kind = UsdTweakAddon::Kind::Window;
    addon.draw = &DrawTwiki;
    UsdTweakAddonRegistry::GetInstance().Add(std::move(addon));
}
