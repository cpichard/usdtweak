#include "FileBrowser.h"
#include "Gui.h"
#include "Playblast.h"
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>

#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#define GHC_WITH_EXCEPTIONS 0
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

PXR_NAMESPACE_USING_DIRECTIVE

std::string PlayblastModalDialog::directory = "";
std::string PlayblastModalDialog::filenamePrefix = "";
int PlayblastModalDialog::start = -1;
int PlayblastModalDialog::end = -1;
int PlayblastModalDialog::width = 960;

PlayblastModalDialog::PlayblastModalDialog(UsdStagePtr stage) : _stage(stage) {
    if (directory.empty()) {
        directory = fs::temp_directory_path().string(); // TODO : check it works with macOS and linux
    }
    if (filenamePrefix.empty()) {
        filenamePrefix = "playblast";
    }
    if (start == -1 && end == -1) {
        start = static_cast<int>(_stage->GetStartTimeCode());
        end = static_cast<int>(_stage->GetEndTimeCode());
    }
    // find all camera in the stqge
    if (stage) {
        for (const auto &prim : stage->Traverse()) {
            if (prim.IsA<UsdGeomCamera>()) {
                _stageCameras.push_back(prim.GetPath());
            }
        }
    }
    // Select the first camera
    if (!_stageCameras.empty()) {
        _cameraPath = _stageCameras[0];
    }
};

void PlayblastModalDialog::Draw() {
    // Draw available cameras
    const char *selectedCameraName = _cameraPath == SdfPath() ? "No camera" : _cameraPath.GetText();
    if (ImGui::BeginCombo("Stage camera", selectedCameraName)) {
        for (const SdfPath &stageCameraPath : _stageCameras) {
            if (ImGui::Selectable(stageCameraPath.GetText())) {
                _cameraPath = stageCameraPath;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::InputText("Output directory", &directory);
    ImGui::InputText("Output image prefix", &filenamePrefix);
    ImGui::Checkbox("Render sequence", &isSequence);
    if (isSequence) {
        ImGui::InputInt("Start", &start);
        ImGui::InputInt("End", &end);
    }
    ImGui::InputInt("Image width", &width);

    if (!directory.empty() && !filenamePrefix.empty() && start <= end && _cameraPath != SdfPath()) {
        ImGui::Text("Rendering to : %s\\%s.#.jpg", directory.c_str(), filenamePrefix.c_str());
        if (ImGui::Button("Blast")) {
            if (width > 0) {
                _recorder.SetImageWidth(width);
            }
            _recorder.SetRendererPlugin(TfToken("HdStormRendererPlugin"));
            _recorder.SetColorCorrectionMode(TfToken("sRGB"));
            _recorder.SetComplexity(0.0);
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
        if (ImGui::Button("Cancel")) {
            CloseModal();
        }
    }
}
