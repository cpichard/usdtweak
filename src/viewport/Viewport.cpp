#include <iostream>

#include <pxr/imaging/garch/glApi.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/boundable.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include "Gui.h"
#include "ImGuiHelpers.h"
#include "Viewport.h"
#include "Commands.h"
#include "Constants.h"
#include "Shortcuts.h"
#include "UsdPrimEditor.h" // DrawUsdPrimEditTarget

namespace clk = std::chrono;

// TODO: picking meshes: https://groups.google.com/g/usd-interest/c/P2CynIu7MYY/m/UNPIKzmMBwAJ

Viewport::Viewport(UsdStageRefPtr stage, Selection &selection)
    : _stage(stage), _cameraManipulator({InitialWindowWidth, InitialWindowHeight}),
      _currentEditingState(new MouseHoverManipulator()), _activeManipulator(&_positionManipulator), _selection(selection),
      _textureSize(1, 1), _viewportName("Viewport 1") {

    // Viewport draw target
    _cameraManipulator.ResetPosition(GetEditableCamera());

    _drawTarget = GlfDrawTarget::New(_textureSize, false);
    _drawTarget->Bind();
    _drawTarget->AddAttachment("color", GL_RGBA, GL_FLOAT, GL_RGBA);
    _drawTarget->AddAttachment("depth", GL_DEPTH_COMPONENT, GL_FLOAT, GL_DEPTH_COMPONENT32F);
    auto color = _drawTarget->GetAttachment("color");
    _textureId = color->GetGlTextureName();
    _drawTarget->Unbind();
}

Viewport::~Viewport() {
    if (_renderer) {
        _renderer = nullptr; // will be deleted in the map
    }
    // Delete renderers
    _drawTarget->Bind();
    for (auto &renderer : _renderers) {
        // Warning, InvalidateBuffers might be defered ... :S to check
        // removed in 20.11: renderer.second->InvalidateBuffers();
        if (renderer.second) {
            delete renderer.second;
            renderer.second = nullptr;
        }
    }
    _drawTarget->Unbind();
    _renderers.clear();

}

static void DrawOpenedStages() {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    const UsdStageCache &stageCache = UsdUtilsStageCache::Get();
    const auto allStages = stageCache.GetAllStages();
    for (const auto &stagePtr : allStages) {
        if (ImGui::MenuItem(stagePtr->GetRootLayer()->GetIdentifier().c_str())) {
            ExecuteAfterDraw<EditorSetCurrentStage>(stagePtr->GetRootLayer());
        }
    }
}


/// Draw the viewport widget
void Viewport::Draw() {
    const ImVec2 wsize = ImGui::GetWindowSize();
    // Set the size of the texture here as we need the current window size
    const auto cursorPos = ImGui::GetCursorPos();
    _textureSize = GfVec2i(std::max(1.f, wsize[0]),
                           std::max(1.f, wsize[1] - cursorPos.y));

    if (_textureId) {
        // Get the size of the child (i.e. the whole draw size of the windows).
        ImGui::Image((ImTextureID)((uintptr_t)_textureId), ImVec2(_textureSize[0], _textureSize[1]), ImVec2(0, 1), ImVec2(1, 0));
        // TODO: it is possible to have a popup menu on top of the viewport.
        // It should be created depending on the manipulator/editor state
        //if (ImGui::BeginPopupContextItem()) {
        //    ImGui::Button("ColorCorrection");
        //    ImGui::Button("Deactivate");
        //    ImGui::EndPopup();
        //}
        HandleManipulationEvents();
        HandleKeyboardShortcut();
        ImGui::BeginDisabled(!bool(GetCurrentStage()));
        DrawToolBar(cursorPos + ImVec2(120, 15));
        DrawManipulatorToolbox(cursorPos + ImVec2(15, 15));
        ImGui::EndDisabled();
    }
}

void Viewport::DrawToolBar(const ImVec2 widgetPosition) {
    const ImVec2 buttonSize(25, 25); // Button size
    const ImVec4 defaultColor(0.1, 0.1, 0.1, 0.7);
    const ImVec4 selectedColor(ColorButtonHighlight);

    ImGui::SetCursorPos(widgetPosition);
    ImGui::PushStyleColor(ImGuiCol_Button, defaultColor);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, defaultColor);
    ImGuiPopupFlags flags = ImGuiPopupFlags_MouseButtonLeft;
    
    ImGui::Button(ICON_FA_USER_COG);
    if (_renderer && ImGui::BeginPopupContextItem(nullptr, flags)) {
        DrawRendererControls(*_renderer);
        DrawRendererSelectionCombo(*_renderer);
        DrawColorCorrection(*_renderer, _imagingSettings);
        DrawAovSettings(*_renderer);
        DrawRendererCommands(*_renderer);
        if (ImGui::BeginMenu("Renderer Settings")) {
            DrawRendererSettings(*_renderer, _imagingSettings);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 1) {
        ImGui::SetTooltip("Renderer settings");
    }
    ImGui::SameLine();
    ImGui::Button(ICON_FA_TV);
    if (_renderer && ImGui::BeginPopupContextItem(nullptr, flags)) {
        DrawImagingSettings(*_renderer, _imagingSettings);
        ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 1) {
        ImGui::SetTooltip("Viewport settings");
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, _imagingSettings.enableCameraLight ? selectedColor : defaultColor);
    if (ImGui::Button(ICON_FA_FIRE)) {
        _imagingSettings.enableCameraLight = !_imagingSettings.enableCameraLight;
    }
    if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 1) {
        ImGui::SetTooltip("Enable camera light");
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, _imagingSettings.enableSceneMaterials ? selectedColor : defaultColor);
    if (ImGui::Button(ICON_FA_HAND_SPARKLES)) {
        _imagingSettings.enableSceneMaterials = !_imagingSettings.enableSceneMaterials;
    }
    if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 1) {
        ImGui::SetTooltip("Enable scene materials");
    }
    ImGui::PopStyleColor();
    if (_renderer && _renderer->GetRendererPlugins().size() >= 2) {
        ImGui::SameLine();
        ImGui::Button(_renderer->GetRendererDisplayName(_renderer->GetCurrentRendererId()).c_str());
        if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 1) {
            ImGui::SetTooltip("Render delegate");
        }
        if (ImGui::BeginPopupContextItem(nullptr, flags)) {
            DrawRendererSelectionList(*_renderer);
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();
    std::string cameraName(ICON_FA_CAMERA);
    cameraName += "  " + _cameras.GetCurrentCameraName();
    ImGui::Button(cameraName.c_str());
    if (_renderer && ImGui::BeginPopupContextItem(_viewportName.c_str(), flags)) { // should be name with the viewport name instead
        _cameras.DrawCameraList(GetCurrentStage());
        _cameras.DrawCameraEditor(GetCurrentStage(), GetCurrentTimeCode());
        ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered() && GImGui->HoveredIdTimer > 1) {
        ImGui::SetTooltip("Cameras");
    }
    ImGui::PopStyleColor(2);
}


// Poor man manipulator toolbox
void Viewport::DrawManipulatorToolbox(const ImVec2 widgetPosition) {
    const ImVec2 buttonSize(25, 25); // Button size
    const ImVec4 defaultColor(0.1, 0.1, 0.1, 0.9);
    const ImVec4 selectedColor(ColorButtonHighlight);
    ImGui::SetCursorPos(widgetPosition);
    ImGui::SetNextItemWidth(80);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, defaultColor);
    DrawPickMode(_selectionManipulator);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Button, IsChosenManipulator<MouseHoverManipulator>() ? selectedColor : defaultColor);
    ImGui::SetCursorPosX(widgetPosition.x);
    
    if (ImGui::Button(ICON_FA_LOCATION_ARROW, buttonSize)) {
        ChooseManipulator<MouseHoverManipulator>();
    }
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Button, IsChosenManipulator<PositionManipulator>() ? selectedColor : defaultColor);
    ImGui::SetCursorPosX(widgetPosition.x);
    if (ImGui::Button(ICON_FA_ARROWS_ALT, buttonSize)) {
        ChooseManipulator<PositionManipulator>();
    }
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Button, IsChosenManipulator<RotationManipulator>() ? selectedColor : defaultColor);
    ImGui::SetCursorPosX(widgetPosition.x);
    if (ImGui::Button(ICON_FA_SYNC_ALT, buttonSize)) {
        ChooseManipulator<RotationManipulator>();
    }
    ImGui::PopStyleColor();

     ImGui::PushStyleColor(ImGuiCol_Button, IsChosenManipulator<ScaleManipulator>() ? selectedColor : defaultColor);
     ImGui::SetCursorPosX(widgetPosition.x);
    if (ImGui::Button(ICON_FA_COMPRESS, buttonSize)) {
        ChooseManipulator<ScaleManipulator>();
    }
     ImGui::PopStyleColor();
}

/// Frane the viewport using the bounding box of the selection
void Viewport::FrameSelection(const Selection &selection) { // Camera manipulator ???
    if (GetCurrentStage() && !selection.IsSelectionEmpty(GetCurrentStage())) {
        UsdGeomBBoxCache bboxcache(_imagingSettings.frame, UsdGeomImageable::GetOrderedPurposeTokens());
        GfBBox3d bbox;
        for (const auto &primPath : selection.GetSelectedPaths(GetCurrentStage())) {
            bbox = GfBBox3d::Combine(bboxcache.ComputeWorldBound(GetCurrentStage()->GetPrimAtPath(primPath)), bbox);
        }
        auto defaultPrim = GetCurrentStage()->GetDefaultPrim();
        _cameraManipulator.FrameBoundingBox(GetEditableCamera(), bbox);
    }
}

/// Frame the viewport using the bounding box of the root prim
void Viewport::FrameRootPrim(){
    if (GetCurrentStage()) {
        UsdGeomBBoxCache bboxcache(_imagingSettings.frame, UsdGeomImageable::GetOrderedPurposeTokens());
        auto defaultPrim = GetCurrentStage()->GetDefaultPrim();
        if(defaultPrim){
            _cameraManipulator.FrameBoundingBox(GetEditableCamera(), bboxcache.ComputeWorldBound(defaultPrim));
        } else {
            auto rootPrim = GetCurrentStage()->GetPrimAtPath(SdfPath("/"));
            _cameraManipulator.FrameBoundingBox(GetEditableCamera(), bboxcache.ComputeWorldBound(rootPrim));
        }
    }
}

GfVec2d Viewport::GetPickingBoundarySize() const {
    const GfVec2i renderSize = _drawTarget->GetSize();
    const double width = static_cast<double>(renderSize[0]);
    const double height = static_cast<double>(renderSize[1]);
    return GfVec2d(20.0 / width, 20.0 / height);
}

//
double Viewport::ComputeScaleFactor(const GfVec3d& objectPos, const double multiplier) const {
    double scale = 1.0;
    const auto &frustum = GetCurrentCamera().GetFrustum();
    auto ray = frustum.ComputeRay(GfVec2d(0, 0)); // camera axis
    ray.FindClosestPoint(objectPos, &scale);
    const float focalLength = GetCurrentCamera().GetFocalLength();
    scale /= focalLength == 0 ? 1.f : focalLength;
    scale /= multiplier;
    scale *= 2;
    return scale;
}

inline bool IsModifierDown() {
    return ImGui::GetIO().KeyMods != 0;
}

void Viewport::HandleKeyboardShortcut() {
    if (ImGui::IsItemHovered()) {
        ImGuiIO &io = ImGui::GetIO();
        static bool SelectionManipulatorPressedOnce = true;
        if (ImGui::IsKeyDown(ImGuiKey_Q) && ! IsModifierDown() ) {
            if (SelectionManipulatorPressedOnce) {
                ChooseManipulator<MouseHoverManipulator>();
                SelectionManipulatorPressedOnce = false;
            }
        } else {
            SelectionManipulatorPressedOnce = true;
        }

        static bool PositionManipulatorPressedOnce = true;
        if (ImGui::IsKeyDown(ImGuiKey_W) && ! IsModifierDown() ) {
            if (PositionManipulatorPressedOnce) {
                ChooseManipulator<PositionManipulator>();
                PositionManipulatorPressedOnce = false;
            }
        } else {
            PositionManipulatorPressedOnce = true;
        }

        static bool RotationManipulatorPressedOnce = true;
        if (ImGui::IsKeyDown(ImGuiKey_E) && ! IsModifierDown() ) {
            if (RotationManipulatorPressedOnce) {
                ChooseManipulator<RotationManipulator>();
                RotationManipulatorPressedOnce = false;
            }
        } else {
            RotationManipulatorPressedOnce = true;
        }

        static bool ScaleManipulatorPressedOnce = true;
        if (ImGui::IsKeyDown(ImGuiKey_R) && ! IsModifierDown() ) {
            if (ScaleManipulatorPressedOnce) {
                ChooseManipulator<ScaleManipulator>();
                ScaleManipulatorPressedOnce = false;
            }
        } else {
            ScaleManipulatorPressedOnce = true;
        }
        if (_playing) {
            AddShortcut<EditorStopPlayback, ImGuiKey_Space>();
        } else {
            AddShortcut<EditorStartPlayback, ImGuiKey_Space>();
        }
    }
}


void Viewport::HandleManipulationEvents() {

    ImGuiContext *g = ImGui::GetCurrentContext();
    ImGuiIO &io = ImGui::GetIO();

    // Check the mouse is over this widget
    if (ImGui::IsItemHovered()) {
        const GfVec2i drawTargetSize = _drawTarget->GetSize();
        if (drawTargetSize[0] == 0 || drawTargetSize[1] == 0) return;
        _mousePosition[0] = 2.0 * (static_cast<double>(io.MousePos.x - (g->LastItemData.Rect.Min.x)) /
            static_cast<double>(drawTargetSize[0])) -
            1.0;
        _mousePosition[1] = -2.0 * (static_cast<double>(io.MousePos.y - (g->LastItemData.Rect.Min.y)) /
            static_cast<double>(drawTargetSize[1])) +
            1.0;

        /// This works like a Finite state machine
        /// where every manipulator/editor is a state
        if (!_currentEditingState){
            _currentEditingState = GetManipulator<MouseHoverManipulator>();
            _currentEditingState->OnBeginEdition(*this);
        }

        auto newState = _currentEditingState->OnUpdate(*this);
        if (newState != _currentEditingState) {
            _currentEditingState->OnEndEdition(*this);
            _currentEditingState = newState;
            _currentEditingState->OnBeginEdition(*this);
        }
    } else { // Mouse is outside of the viewport, reset the state
        if (_currentEditingState) {
            _currentEditingState->OnEndEdition(*this);
            _currentEditingState = nullptr;
        }
    }
}


GfCamera &Viewport::GetEditableCamera() { return _cameras.GetEditableCamera(); }
const GfCamera &Viewport::GetCurrentCamera() const { return _cameras.GetCurrentCamera(); }


void Viewport::BeginHydraUI(int width, int height) {
    // Create a ImGui windows to render the gizmos in
    ImGui_ImplOpenGL3_NewFrame();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)width, (float)height);
    ImGui::NewFrame();
    static bool alwaysOpened = true;
    constexpr ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_None;
    constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
                                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGuiViewport *viewport = ImGui::GetMainViewport();
    // Full screen invisible window
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowBgAlpha(0.0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("HydraHUD", &alwaysOpened, windowFlags);
    ImGui::PopStyleVar(3);
}

void Viewport ::EndHydraUI() { ImGui::End(); }

void Viewport::Render() {
    GfVec2i renderSize = _drawTarget->GetSize();
    int width = renderSize[0];
    int height = renderSize[1];

    if (width == 0 || height == 0)
        return;

    // Draw active manipulator and HUD
    BeginHydraUI(width, height);
    GetActiveManipulator().OnDrawFrame(*this);
    // DrawHUD(this);
    EndHydraUI();

    _drawTarget->Bind();
    glEnable(GL_DEPTH_TEST);
    glClearColor(_imagingSettings.clearColor[0], _imagingSettings.clearColor[1], _imagingSettings.clearColor[2],
                 _imagingSettings.clearColor[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, width, height);

    if (_renderer && GetCurrentStage()) {
        // Render hydra
        // Set camera and lighting state
        _imagingSettings.SetLightPositionFromCamera(GetCurrentCamera());
        _renderer->SetLightingState(_imagingSettings.GetLights(), _imagingSettings._material, _imagingSettings._ambient);
        
        // Clipping planes
        _imagingSettings.clipPlanes.clear();
        for (int i = 0; i < GetCurrentCamera().GetClippingPlanes().size(); ++i) {
            _imagingSettings.clipPlanes.emplace_back(GetCurrentCamera().GetClippingPlanes()[i]); // convert float to double
        }
        
        GfVec4d viewport(0, 0, width, height);
        GfRect2i renderBufferRect(GfVec2i(0, 0), width, height);
        GfRange2f displayWindow(GfVec2f(viewport[0], height-viewport[1]-viewport[3]),
                                GfVec2f(viewport[0]+viewport[2],height-viewport[1]));
        GfRect2i dataWindow = renderBufferRect.GetIntersection(
                                                 GfRect2i(GfVec2i(viewport[0], height-viewport[1]-viewport[3]),
                                                              viewport[2], viewport[3]             ));
        CameraUtilFraming framing(displayWindow, dataWindow);
        _renderer->SetRenderBufferSize(renderSize);
        _renderer->SetFraming(framing);
        _renderer->SetOverrideWindowPolicy(std::make_pair(true, CameraUtilConformWindowPolicy::CameraUtilMatchVertically));

        if (_cameras.IsUsingStageCamera()) {
            _renderer->SetCameraPath(_cameras.GetStageCameraPath());
        } else {
            _renderer->SetCameraState(GetCurrentCamera().GetFrustum().ComputeViewMatrix(),
                                  GetCurrentCamera().GetFrustum().ComputeProjectionMatrix());
        }
        _renderer->Render(GetCurrentStage()->GetPseudoRoot(), _imagingSettings);
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // Draw grid. TODO: this should be in a usd render task
    // TODO the grid should handle the ortho case
    if (_imagingSettings.showGrid) {
        _grid.Render(*this);
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    _drawTarget->Unbind();
}

void Viewport::SetCurrentTimeCode(const UsdTimeCode &tc) {
    _imagingSettings.frame = tc;
}

/// Update anything that could have change after a frame render
void Viewport::Update() {
    if (GetCurrentStage()) {
        auto whichRenderer = _renderers.find(GetCurrentStage()); /// We expect a very limited number of opened stages
        if (whichRenderer == _renderers.end()) {
            SdfPathVector excludedPaths;
            _renderer = new UsdImagingGLEngine(GetCurrentStage()->GetPseudoRoot().GetPath(), excludedPaths);
            if (_renderers.empty()) {
                FrameRootPrim();
            }
            _renderers[GetCurrentStage()] = _renderer;
            _cameraManipulator.SetZIsUp(UsdGeomGetStageUpAxis(GetCurrentStage()) == "Z");
            _grid.SetZIsUp(UsdGeomGetStageUpAxis(GetCurrentStage()) == "Z");
            InitializeRendererAov(*_renderer);
        } else if (whichRenderer->second != _renderer) {
            _renderer = whichRenderer->second;
            _cameraManipulator.SetZIsUp(UsdGeomGetStageUpAxis(GetCurrentStage()) == "Z");
            // TODO: should reset the camera otherwise, depending on the position of the camera, the transform is incorrect
            _grid.SetZIsUp(UsdGeomGetStageUpAxis(GetCurrentStage()) == "Z");
            // TODO: the selection is also different per stage
            //_selection =
        }

        // This should be extracted in a Playback module,
        // also the current code is not providing the exact frame rate, it doesn't take into account when the frame is
        // displayed. This is a first implementation to get an idea of how it should interact with the rest of the application.
        if (_playing) {
            auto current = clk::steady_clock::now();
            const auto timesCodePerSec = GetCurrentStage()->GetTimeCodesPerSecond();
            const auto timeDifference = std::chrono::duration<double>(current - _lastFrameTime);
            double newFrame =
                _imagingSettings.frame.GetValue() + timesCodePerSec * timeDifference.count(); // for now just increment the frame
            if (newFrame > GetCurrentStage()->GetEndTimeCode()) {
                newFrame = GetCurrentStage()->GetStartTimeCode();
            } else if (newFrame < GetCurrentStage()->GetStartTimeCode()) {
                newFrame = GetCurrentStage()->GetStartTimeCode();
            }
            _imagingSettings.frame = UsdTimeCode(newFrame);
            _lastFrameTime = current;
        }

        // Update cameras state, this will assign the user selected camera for the current stage at
        // a particular time
        _cameras.Update(GetCurrentStage(), GetCurrentTimeCode());
    }

    const GfVec2i &currentSize = _drawTarget->GetSize();
    if (currentSize != _textureSize) {
        _drawTarget->Bind();
        _drawTarget->SetSize(_textureSize);
        _drawTarget->Unbind();

    }

    // We have to set the camera aspect ratio at each frame as the stage camera might have a different one
    // and the user can resize the viewport
    _cameras.SetCameraAspectRatio(_textureSize[0], _textureSize[1]);

    if (_renderer && _selection.UpdateSelectionHash(GetCurrentStage(), _lastSelectionHash)) {
        _renderer->ClearSelected();
        _renderer->SetSelected(_selection.GetSelectedPaths(GetCurrentStage()));

        // Tell the manipulators the selection has changed
        _positionManipulator.OnSelectionChange(*this);
        _rotationManipulator.OnSelectionChange(*this);
        _scaleManipulator.OnSelectionChange(*this);
    }
}


bool Viewport::TestIntersection(GfVec2d clickedPoint, SdfPath &outHitPrimPath, SdfPath &outHitInstancerPath, int &outHitInstanceIndex) {

    GfVec2i renderSize = _drawTarget->GetSize();
    double width = static_cast<double>(renderSize[0]);
    double height = static_cast<double>(renderSize[1]);

    GfFrustum pixelFrustum = GetCurrentCamera().GetFrustum().ComputeNarrowedFrustum(clickedPoint, GfVec2d(1.0 / width, 1.0 / height));
    GfVec3d outHitPoint;
    GfVec3d outHitNormal;
    return (_renderer && GetCurrentStage() && _renderer->TestIntersection(GetCurrentCamera().GetFrustum().ComputeViewMatrix(),
            pixelFrustum.ComputeProjectionMatrix(),
            GetCurrentStage()->GetPseudoRoot(), _imagingSettings, &outHitPoint, &outHitNormal,
            &outHitPrimPath, &outHitInstancerPath, &outHitInstanceIndex));
}


void Viewport::StartPlayback() {
    _playing = true;
    _lastFrameTime = clk::steady_clock::now();
}

void Viewport::StopPlayback() {
    _playing = false;
    // cast to nearest frame
    _imagingSettings.frame = UsdTimeCode(int(_imagingSettings.frame.GetValue()));
}
