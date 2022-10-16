#pragma once

#include <string>

#include <pxr/usdImaging/usdAppUtils/frameRecorder.h>

#include "ModalDialogs.h"

PXR_NAMESPACE_USING_DIRECTIVE

/// Playblast dialog
/// This is a minimal implementation of a playblast dialog using UsdAppUtilsFrameRecorder.
/// It needs some improvements as it's not possible to blast the viewport camera unless it's a stage camera,
/// we can't select the renderer, we can't change options like loading materials or not, change the file format ...
/// Also there is not ui for selecting the output directory, and no progress bar.
///
/// For all of those desirable features we need to extract the code from UsdAppUtilsFrameRecorder into another frame recorder.
///
struct PlayblastModalDialog : public ModalDialog {

    PlayblastModalDialog(UsdStagePtr stage);
    ~PlayblastModalDialog() override {}

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
