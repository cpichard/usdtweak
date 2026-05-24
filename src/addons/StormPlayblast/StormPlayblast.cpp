#include "addons/Api.h"

#include "FileBrowser.h"
#include "Gui.h"

#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usdImaging/usdAppUtils/frameRecorder.h>

#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#define GHC_WITH_EXCEPTIONS 0
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

struct PlayblastModalDialog : public ModalDialog {

    PlayblastModalDialog(UsdStagePtr stage);
    ~PlayblastModalDialog() override = default;

    void Draw() override;
    const char *DialogId() const override { return "Playblast"; }

    UsdAppUtilsFrameRecorder _recorder;
    UsdStagePtr _stage;
    SdfPath _cameraPath;
    SdfPathVector _stageCameras;

    static std::string directory;
    static std::string filenamePrefix;
    bool isSequence = true;
    static int start;
    static int end;
    static int width;
};

std::string PlayblastModalDialog::directory = "";
std::string PlayblastModalDialog::filenamePrefix = "";
int PlayblastModalDialog::start = -1;
int PlayblastModalDialog::end = -1;
int PlayblastModalDialog::width = 960;

PlayblastModalDialog::PlayblastModalDialog(UsdStagePtr stage) : _stage(stage) {
    if (directory.empty()) {
        directory = fs::temp_directory_path().string();
    }
    if (filenamePrefix.empty()) {
        filenamePrefix = "playblast";
    }
    if (start == -1 && end == -1) {
        start = static_cast<int>(_stage->GetStartTimeCode());
        end = static_cast<int>(_stage->GetEndTimeCode());
    }
    if (stage) {
        for (const auto &prim : stage->Traverse()) {
            if (prim.IsA<UsdGeomCamera>()) {
                _stageCameras.push_back(prim.GetPath());
            }
        }
    }
    if (!_stageCameras.empty()) {
        _cameraPath = _stageCameras[0];
    }
}

void PlayblastModalDialog::Draw() {
    const char *selectedCameraName = _cameraPath == SdfPath() ? "No camera" : _cameraPath.GetText();
    if (ImGui::BeginCombo("Stage camera", selectedCameraName)) {
        for (const SdfPath &stageCameraPath : _stageCameras) {
            if (ImGui::Selectable(stageCameraPath.GetText())) {
                _cameraPath = stageCameraPath;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Text("Scene materials ON");
    ImGui::Text("Purposes: default+proxy");
    ImGui::InputText("Output directory", &directory);
    ImGui::InputText("Output image prefix", &filenamePrefix);
    ImGui::Checkbox("Render sequence", &isSequence);
    if (isSequence) {
        ImGui::InputInt("Start", &start);
        ImGui::InputInt("End", &end);
    }
    ImGui::InputInt("Image width", &width);

    ImGui::BeginDisabled(directory.empty() || filenamePrefix.empty() || start > end || _cameraPath == SdfPath());
    ImGui::Text("Rendering to : %s\\%s.#.jpg", directory.c_str(), filenamePrefix.c_str());
    if (ImGui::Button("Blast")) {
        if (width > 0) {
            _recorder.SetImageWidth(width);
        }
        _recorder.SetRendererPlugin(TfToken("HdStormRendererPlugin"));
        _recorder.SetColorCorrectionMode(TfToken("sRGB"));
        _recorder.SetComplexity(1.0);
        UsdGeomCamera camera(_stage->GetPrimAtPath(_cameraPath));
        if (isSequence) {
            for (int i = start; i <= end; ++i) {
                std::string frameName = filenamePrefix + "." + std::to_string(i) + ".jpg";
                fs::path outputFrame(directory);
                if (fs::is_directory(outputFrame)) {
                    outputFrame /= frameName;
                    _recorder.Record(_stage, camera, UsdTimeCode(i), outputFrame.string());
                }
            }
        } else {
            std::string frameName = filenamePrefix + "." + ".jpg";
            fs::path outputFrame(directory);
            if (fs::is_directory(outputFrame)) {
                outputFrame /= frameName;
                _recorder.Record(_stage, camera, UsdTimeCode::Default(), outputFrame.string());
            }
        }
        CloseModal();
    }
    ImGui::SameLine();
    ImGui::EndDisabled();

    if (ImGui::Button("Cancel")) {
        CloseModal();
    }
}

bool IsStormAvailable() {
    HfPluginDescVector descs;
    HdRendererPluginRegistry::GetInstance().GetPluginDescs(&descs);
    for (const auto &desc : descs) {
        if (desc.id == TfToken("HdStormRendererPlugin")) return true;
    }
    return false;
}

} // namespace

TF_REGISTRY_FUNCTION_WITH_TAG(UsdTweakAddonRegistry, StormPlayblast) {
    UsdTweakAddon addon;
    addon.id = "StormPlayblast";
    addon.menuLabel = ICON_FA_IMAGES " Storm playblast";
    addon.kind = UsdTweakAddon::Kind::Action;
    addon.activate = []() {
        auto stage = usdtweak::GetCurrentStage();
        if (stage) DrawModalDialog<PlayblastModalDialog>(stage);
    };
    addon.isAvailable = []() {
        return usdtweak::GetCurrentStage() && IsStormAvailable();
    };
    UsdTweakAddonRegistry::GetInstance().Add(std::move(addon));
}
