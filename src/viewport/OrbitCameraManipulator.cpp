#include "OrbitCameraManipulator.h"
#include "Commands.h"
#include "Editor.h"
#include "Gui.h"
#include "Viewport.h"
#include <pxr/usd/usdGeom/camera.h>

OrbitCameraManipulator::OrbitCameraManipulator(const GfVec2i &viewportSize, bool isZUp) : CameraRig(viewportSize, isZUp) {}

void OrbitCameraManipulator::OnBeginEdition(Viewport &viewport) {
    if (viewport.IsEditingStageCamera()) {
        _stageCamera = UsdGeomCamera::Get(viewport.GetCurrentStage(), viewport.GetSelectedStageCameraPath());
        BeginEdition(viewport.GetCurrentStage());
    }
}

void OrbitCameraManipulator::OnEndEdition(Viewport &viewport) {
    if (viewport.IsEditingStageCamera() && _stageCamera) {
        EndEdition();
    }
    SetMovementType(MovementType::None);
}

Manipulator *OrbitCameraManipulator::OnUpdate(Viewport &viewport) {
    auto &cameraManipulator = viewport.GetOrbitCameraManipulator();
    ImGuiIO &io = ImGui::GetIO();
    SetViewportSize(viewport.GetViewportSize());
    if (!ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
        Editor::SetMouseCaptured(false);
        return viewport.GetManipulator<MouseHoverManipulator>();
    } else if (ImGui::IsMouseReleased(1) || ImGui::IsMouseReleased(2) || ImGui::IsMouseReleased(0)) {
        Editor::SetMouseCaptured(false);
        SetMovementType(MovementType::None);
    } else if (ImGui::IsMouseClicked(0) && !viewport.IsEditingInternalOrthoCamera()) {
        Editor::SetMouseCaptured(true);
        SetMovementType(MovementType::Orbit);
    } else if (ImGui::IsMouseClicked(2)) {
        Editor::SetMouseCaptured(true);
        SetMovementType(MovementType::Truck);
    } else if (ImGui::IsMouseClicked(1)) {
        Editor::SetMouseCaptured(true);
        SetMovementType(MovementType::Dolly);
    }
    GfCamera &currentCamera = viewport.GetEditableCamera();
    GfVec2d mouseDelta(io.MouseDelta.x, io.MouseDelta.y);
    if (Move(currentCamera, mouseDelta)) {
        if (viewport.IsEditingStageCamera() && _stageCamera) {
            // This is going to fill the undo/redo buffer :S
            _stageCamera.SetFromCamera(currentCamera, viewport.GetCurrentTimeCode());
        }
    }
    return this;
}

bool OrbitCameraManipulator::Move(GfCamera &camera, const GfVec2d &delta) {
    GfQuatd rotation;
    GfVec3d center;
    GetCameraTransform(camera, center, rotation, _dist);

    // convert pixels (deltaX/Y) to degrees
    const double dtFactor = 0.15;

    if (_movementType == MovementType::Orbit) { // TUMBLE
        GfVec2d yp = GetYawPitch(camera);
        GfRotation rotY(GfVec3d::YAxis(), yp[0] - (delta[0] * dtFactor));
        GfRotation rotX(GfVec3d::XAxis(), yp[1] - (delta[1] * dtFactor));
        SetCameraTransform(camera, center, rotY.GetQuat() * rotX.GetQuat(), _dist);
    } else if (_movementType == MovementType::Truck) {
        auto frustum = camera.GetFrustum();
        GfVec3d up, right, cameraAxis;
        GetCameraVectors(camera, right, up, cameraAxis);
        double pixelToWorld = 1.0;
        const GfRange2d &window = frustum.GetWindow();
        if (camera.GetProjection() == GfCamera::Orthographic) {
            pixelToWorld = window.GetSize()[0] / static_cast<double>(_viewportSize[0]);
        } else {
            pixelToWorld = window.GetSize()[0] * _dist / static_cast<double>(_viewportSize[0]);
        }
        center += -delta[0] * right * pixelToWorld + delta[1] * up * pixelToWorld;
        SetCameraTransform(camera, center, rotation, _dist);
    } else if (_movementType == MovementType::Dolly) { // Not really a dolly in the orthographic case
        auto scaleFactor = 1.0 + -0.002 * (delta[0] + delta[1]);
        if (camera.GetProjection() == GfCamera::Orthographic) {
            auto value = camera.GetHorizontalAperture() * GfCamera::APERTURE_UNIT * (scaleFactor);
            camera.SetOrthographicFromAspectRatioAndSize(camera.GetAspectRatio(), value, GfCamera::FOVHorizontal);
        } else {
            if (scaleFactor > 1.0 && _dist < 2.0) {
                const auto selBasedIncr = _selectionSize / 25.0;
                scaleFactor -= 1.0;
                _dist += std::min(selBasedIncr, scaleFactor);
            } else {
                _dist *= scaleFactor;
            }
            SetCameraTransform(camera, center, rotation, _dist);
        }
    } else {
        return false; // NO updates
    }
    return true;
}
