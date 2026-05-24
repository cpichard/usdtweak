#include "addons/Api.h"

#include "Gui.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <string>
#include <vector>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

constexpr const char *kAddonId = "LauncherBar";

// ---------- Persistence ----------------------------------------------------
// Stored as:
//   Addon.LauncherBar.count=<N>
//   Addon.LauncherBar.launcher.<i>.name=<name>
//   Addon.LauncherBar.launcher.<i>.cmd=<commandline>

struct Launcher {
    std::string name;
    std::string cmd;
};

std::string IndexKey(int i, const char *field) {
    return "launcher." + std::to_string(i) + "." + field;
}

std::vector<Launcher> LoadLaunchers() {
    std::vector<Launcher> out;
    const std::string countStr = usdtweak::GetAddonString(kAddonId, "count", "0");
    const int count = std::atoi(countStr.c_str());
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        Launcher l;
        l.name = usdtweak::GetAddonString(kAddonId, IndexKey(i, "name"), "");
        l.cmd = usdtweak::GetAddonString(kAddonId, IndexKey(i, "cmd"), "");
        if (!l.name.empty()) out.push_back(std::move(l));
    }
    return out;
}

void SaveLaunchers(const std::vector<Launcher> &launchers) {
    usdtweak::SetAddonString(kAddonId, "count", std::to_string(launchers.size()));
    for (size_t i = 0; i < launchers.size(); ++i) {
        usdtweak::SetAddonString(kAddonId, IndexKey(i, "name"), launchers[i].name);
        usdtweak::SetAddonString(kAddonId, IndexKey(i, "cmd"), launchers[i].cmd);
    }
}

// ---------- Execution ------------------------------------------------------
// Holds futures so async tasks aren't joined prematurely.
std::vector<std::future<int>> &PendingTasks() {
    static std::vector<std::future<int>> tasks;
    return tasks;
}

void RunLauncher(const std::string &cmdTemplate) {
    std::string cmd = cmdTemplate;

    auto stage = usdtweak::GetCurrentStage();
    auto layer = usdtweak::GetCurrentLayer();
    const std::string stagePath = (stage && stage->GetRootLayer()) ? stage->GetRootLayer()->GetRealPath() : "";
    const std::string layerPath = layer ? layer->GetRealPath() : "";

    auto replaceAll = [&](const char *token, const std::string &value) {
        const size_t tokLen = std::strlen(token);
        for (size_t pos = cmd.find(token); pos != std::string::npos; pos = cmd.find(token, pos + value.size())) {
            cmd.replace(pos, tokLen, value);
        }
    };
    replaceAll("__STAGE_PATH__", stagePath);
    replaceAll("__LAYER_PATH__", layerPath);

    const UsdTimeCode tc = usdtweak::GetCurrentTimeCode();
    if (!tc.IsDefault()) {
        replaceAll("__CURRENT_TIME__", std::to_string(tc.GetValue()));
    }

    // Drop any tasks that have already finished, so the vector doesn't grow
    // unboundedly. Cheap O(n) scan; the count of in-flight launchers is small.
    auto &tasks = PendingTasks();
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
                               [](std::future<int> &f) {
                                   return f.valid() &&
                                          f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                               }),
                tasks.end());

    tasks.emplace_back(std::async(std::launch::async,
                                  [cmd]() -> int { return std::system(cmd.c_str()); }));
}

// ---------- UI -------------------------------------------------------------
struct AddLauncherDialog : public ModalDialog {
    void Draw() override {
        ImGui::InputText("Launcher name", &_name);
        ImGui::InputText("Command line", &_cmd);
        DrawModalButtonsOkCancel([&]() {
            if (_name.empty() || _cmd.empty()) return;
            auto launchers = LoadLaunchers();
            // Reject duplicates by name.
            for (const auto &l : launchers) {
                if (l.name == _name) return;
            }
            launchers.push_back({_name, _cmd});
            SaveLaunchers(launchers);
        });
    }
    const char *DialogId() const override { return "Add launcher"; }
    std::string _name;
    std::string _cmd;
};

void DrawLauncherBar() {
    if (ImGui::Button("+")) {
        DrawModalDialog<AddLauncherDialog>();
    }
    ImGui::SameLine();

    auto launchers = LoadLaunchers();
    bool changed = false;
    for (size_t i = 0; i < launchers.size(); ++i) {
        const auto &l = launchers[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(l.name.c_str())) {
            if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
                launchers.erase(launchers.begin() + i);
                changed = true;
                ImGui::PopID();
                break;
            } else {
                RunLauncher(l.cmd);
            }
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    if (changed) SaveLaunchers(launchers);
    // TODO hint to say that control click deletes the launcher
}

} // namespace

TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, LauncherBar) {
    UsdTweakAddon addon;
    addon.id = kAddonId;
    addon.menuLabel = "Launcher bar";
    addon.kind = UsdTweakAddon::Kind::Window;
    addon.draw = &DrawLauncherBar;
    UsdTweakAddonRegistry::GetInstance().Add(std::move(addon));
}
