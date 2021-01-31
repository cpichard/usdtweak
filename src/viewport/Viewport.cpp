#include <iostream>

#include <pxr/imaging/glf/glew.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/boundable.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/imageable.h>

#include <GLFW/glfw3.h>
#include "Gui.h"
#include "Viewport.h"
#include "Commands.h"
#include "Constants.h"
#include "RendererSettings.h"

template <typename BasicShaderT>
void DrawBasicShadingProperties(BasicShaderT &shader) {
    auto ambient = shader.GetAmbient();
    ImGui::ColorEdit4("ambient", ambient.data());
    shader.SetAmbient(ambient);

    auto diffuse = shader.GetDiffuse();
    ImGui::ColorEdit4("diffuse", diffuse.data());
    shader.SetDiffuse(diffuse);

    auto specular = shader.GetSpecular();
    ImGui::ColorEdit4("specular", specular.data());
    shader.SetSpecular(specular);
}

void DrawGLLights(GlfSimpleLightVector &lights) {
    for (auto &light : lights) {
        // Light Position
        if (ImGui::TreeNode("Light")) {
            auto position = light.GetPosition();
            ImGui::DragFloat4("position", position.data());
            light.SetPosition(position);
            DrawBasicShadingProperties(light);
            ImGui::TreePop();
        }
    }
}

// The camera path will move once we create a CameraList class in charge of
// camera selection per stage
static SdfPath perspectiveCameraPath("/usdtweak/cameras/cameraPerspective");

/// Draw a camera selection
void DrawCameraList(Viewport &viewport) {
    // TODO: the viewport cameras and the stage camera should live in different lists
    constexpr char const *perspectiveCameraName = "Perspective";
    if (ImGui::ListBoxHeader("")) {
        // OpenGL Cameras
        if (ImGui::Selectable(perspectiveCameraName, viewport.GetCameraPath() == perspectiveCameraPath)) {
            viewport.SetCameraPath(perspectiveCameraPath);
        }
        if (viewport.GetCurrentStage()) {
            UsdPrimRange range = viewport.GetCurrentStage()->Traverse();
            for (const auto &prim : range) {
                if (prim.IsA<UsdGeomCamera>()) {
                    const bool isSelected = (prim.GetPath() == viewport.GetCameraPath());
                    if (ImGui::Selectable(prim.GetName().data(), isSelected)) {
                        viewport.SetCameraPath(prim.GetPath());
                    }
                }
            }
        }
        ImGui::ListBoxFooter();
    }

    // Draw focalLength
    auto & camera = viewport.GetCurrentCamera();
    float focal = camera.GetFocalLength();
    ImGui::InputFloat("Focal length", &focal);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        camera.SetFocalLength(focal);
    }
}


Viewport::Viewport(UsdStageRefPtr stage, Selection &selection)
    : _stage(stage), _cameraManipulator({InitialWindowWidth, InitialWindowHeight}),
      _currentEditingState(new MouseHoverManipulator()), _activeManipulator(&_positionManipulator), _selection(selection),
      _viewportSize(InitialWindowWidth, InitialWindowHeight), _selectedCameraPath(perspectiveCameraPath), _renderCamera(&_perspectiveCamera) {

    // Viewport draw target
    _cameraManipulator.ResetPosition(GetCurrentCamera());

    _drawTarget = GlfDrawTarget::New(_viewportSize, false);
    _drawTarget->Bind();
    _drawTarget->AddAttachment("color", GL_RGBA, GL_FLOAT, GL_RGBA);
    _drawTarget->AddAttachment("depth", GL_DEPTH_COMPONENT, GL_FLOAT, GL_DEPTH_COMPONENT32F);
    auto color = _drawTarget->GetAttachment("color");
    _textureId = color->GetGlTextureName();
    _drawTarget->Unbind();

    // USD render engine setup
    _renderparams = new UsdImagingGLRenderParams;
    _renderparams->frame = 1.0;
    _renderparams->complexity = 1.0;
    _renderparams->clearColor = GfVec4f(0.5, 0.5, 0.5, 1.0);
    _renderparams->showRender = false;
    _renderparams->forceRefresh = false;
    _renderparams->enableLighting = true;
    _renderparams->enableSceneMaterials = true;
    _renderparams->drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
    _renderparams->highlight = true;
    _renderparams->gammaCorrectColors = false;
    _renderparams->colorCorrectionMode = TfToken("sRGB");
    _renderparams->showGuides = true;
    _renderparams->showProxy = true;
    _renderparams->showRender = false;

    // Lights
    GlfSimpleLight simpleLight;
    simpleLight.SetAmbient({0.2, 0.2, 0.2, 1.0});
    simpleLight.SetDiffuse({1.0, 1.0, 1.0, 1.f});
    simpleLight.SetSpecular({0.2, 0.2, 0.2, 1.f});
    simpleLight.SetPosition({200, 200, 200, 1.0});
    _lights.emplace_back(simpleLight);

    // TODO: set color correction as well

    // Default material
    _material.SetAmbient({0.0, 0.0, 0.0, 1.f});
    _material.SetDiffuse({1.0, 1.0, 1.0, 1.f});
    _material.SetSpecular({0.2, 0.2, 0.2, 1.f});
    _ambient = {0.0, 0.0, 0.0, 0.0};
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
        delete renderer.second;
        renderer.second = nullptr;
    }
    _drawTarget->Unbind();
    _renderers.clear();

    if (_renderparams) {
        delete _renderparams;
        _renderparams = nullptr;
    }

    if (_currentEditingState){
        delete _currentEditingState;
        _currentEditingState = nullptr;
    }
}

/// Draw the viewport widget
void Viewport::Draw() {
    ImVec2 wsize = ImGui::GetWindowSize();
    ImGui::Button("\xef\x80\xb0 Cameras");
    ImGuiPopupFlags flags = ImGuiPopupFlags_MouseButtonLeft;
    if (_renderer && ImGui::BeginPopupContextItem(nullptr, flags)) {
        DrawCameraList(*this);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Button("\xef\x83\xab Lighting");
    if (_renderer && ImGui::BeginPopupContextItem(nullptr, flags)) {
        ImGui::BulletText("Ambient light");
        ImGui::InputFloat4("Ambient", _ambient.data());
        ImGui::Separator();
        DrawGLLights(_lights);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Button("\xef\x87\xbc Shading");
    if (_renderer && ImGui::BeginPopupContextItem(nullptr, flags)) {
        ImGui::BulletText("Default shader");
        DrawBasicShadingProperties(_material);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Button("\xef\x93\xbe Renderer");
    if (_renderer && ImGui::BeginPopupContextItem(nullptr, flags)) {
        DrawRendererSettings(*_renderer, *_renderparams);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Button("\xef\x89\xac Viewport");
    if (_renderer && ImGui::BeginPopupContextItem(nullptr, flags)) {
        DrawOpenGLSettings(*_renderer, *_renderparams);
        ImGui::EndPopup();
    }

    if (_textureId) {
        // Get the size of the child (i.e. the whole draw size of the windows).
        auto cursorPos = ImGui::GetCursorPos();
        ImGui::Image((ImTextureID)_textureId, ImVec2(wsize.x, wsize.y - ViewportBorderSize), ImVec2(0, 1), ImVec2(1, 0));
        // TODO: it is possible to have a popup menu on top of the viewport.
        // It should be created depending on the manipulator/editor state
        //if (ImGui::BeginPopupContextItem()) {
        //    ImGui::Button("ColorCorrection");
        //    ImGui::Button("Deactivate");
        //    ImGui::EndPopup();
        //}
        HandleManipulationEvents();
        HandleKeyboardShortcut();

        DrawManipulatorToolbox(cursorPos);
    }
}

// Poor man manipulator toolbox
void Viewport::DrawManipulatorToolbox(const ImVec2 &cursorPos) {
    const ImVec2 buttonSize(40, 40); // Button size
    const ImVec2 toolBoxPos(20, 20); // Alignment
    const ImVec4 defaultColor(0.1, 0.1, 0.1, 0.9);
    const ImVec4 selectedColor(0.2, 0.2, 0.2, 0.7);

    ImGui::PushStyleColor(ImGuiCol_Button, IsChosenManipulator<MouseHoverManipulator>() ? selectedColor : defaultColor);
    ImGui::SetCursorPos(ImVec2(toolBoxPos.x + cursorPos.x, toolBoxPos.y + cursorPos.y));
    if (ImGui::Button("P", buttonSize)) {
        ChooseManipulator<MouseHoverManipulator>();
    }
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Button, IsChosenManipulator<PositionManipulator>() ? selectedColor : defaultColor);
    ImGui::SetCursorPosX(toolBoxPos.x + cursorPos.x);
    if (ImGui::Button("T", buttonSize)) {
        ChooseManipulator<PositionManipulator>();
    }
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Button, IsChosenManipulator<RotationManipulator>() ? selectedColor : defaultColor);
    ImGui::SetCursorPosX(toolBoxPos.x + cursorPos.x);
    if (ImGui::Button("R", buttonSize)) {
        ChooseManipulator<RotationManipulator>();
    }
    ImGui::PopStyleColor();

     ImGui::PushStyleColor(ImGuiCol_Button, IsChosenManipulator<ScaleManipulator>() ? selectedColor : defaultColor);
     ImGui::SetCursorPosX(toolBoxPos.x + cursorPos.x);
    if (ImGui::Button("S", buttonSize)) {
        ChooseManipulator<ScaleManipulator>();
    }
     ImGui::PopStyleColor();
}

/// Resize the Hydra viewport/render panel
void Viewport::SetSize(int width, int height) {
    _viewportSize = GfVec2i(width, height);
}

/// Frane the viewport using the bounding box of the selection
void Viewport::FrameSelection(const Selection &selection) { // Camera manipulator ???
    if (GetCurrentStage() && selection) {
        UsdGeomBBoxCache bboxcache(_renderparams->frame, UsdGeomImageable::GetOrderedPurposeTokens());
        GfBBox3d bbox;
        for (const auto &primPath : selection->GetAllSelectedPrimPaths()) {
            bbox = GfBBox3d::Combine(bboxcache.ComputeWorldBound(GetCurrentStage()->GetPrimAtPath(primPath)), bbox);
        }
        auto defaultPrim = GetCurrentStage()->GetDefaultPrim();
        _cameraManipulator.FrameBoundingBox(GetCurrentCamera(), bbox);
    }
}

/// Frame the viewport using the bounding box of the root prim
void Viewport::FrameRootPrim(){
    if (GetCurrentStage()) {
        UsdGeomBBoxCache bboxcache(_renderparams->frame, UsdGeomImageable::GetOrderedPurposeTokens());
        auto defaultPrim = GetCurrentStage()->GetDefaultPrim();
        if(defaultPrim){
            _cameraManipulator.FrameBoundingBox(GetCurrentCamera(), bboxcache.ComputeWorldBound(defaultPrim));
        } else {
            auto rootPrim = GetCurrentStage()->GetPrimAtPath(SdfPath("/"));
            _cameraManipulator.FrameBoundingBox(GetCurrentCamera(), bboxcache.ComputeWorldBound(rootPrim));
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

void Viewport::HandleKeyboardShortcut() {
    if (ImGui::IsItemHovered()) {
        ImGuiIO &io = ImGui::GetIO();
        static bool SelectionManipulatorPressedOnce = true;
        if (io.KeysDown[GLFW_KEY_Q]) {
            if (SelectionManipulatorPressedOnce) {
                ChooseManipulator<MouseHoverManipulator>();
                SelectionManipulatorPressedOnce = false;
            }
        } else {
            SelectionManipulatorPressedOnce = true;
        }

        static bool PositionManipulatorPressedOnce = true;
        if (io.KeysDown[GLFW_KEY_W]) {
            if (PositionManipulatorPressedOnce) {
                ChooseManipulator<PositionManipulator>();
                PositionManipulatorPressedOnce = false;
            }
        } else {
            PositionManipulatorPressedOnce = true;
        }

        static bool RotationManipulatorPressedOnce = true;
        if (io.KeysDown[GLFW_KEY_E]) {
            if (RotationManipulatorPressedOnce) {
                ChooseManipulator<RotationManipulator>();
                RotationManipulatorPressedOnce = false;
            }
        } else {
            RotationManipulatorPressedOnce = true;
        }

        static bool ScaleManipulatorPressedOnce = true;
        if (io.KeysDown[GLFW_KEY_R]) {
            if (ScaleManipulatorPressedOnce) {
                ChooseManipulator<ScaleManipulator>();
                ScaleManipulatorPressedOnce = false;
            }
        } else {
            ScaleManipulatorPressedOnce = true;
        }
    }
}


void Viewport::HandleManipulationEvents() {

    ImGuiContext *g = ImGui::GetCurrentContext();
    ImGuiWindow *window = g->CurrentWindow;
    ImGuiIO &io = ImGui::GetIO();

    // Check the mouse is over this widget
    if (ImGui::IsItemHovered()) {
        const GfVec2i drawTargetSize = _drawTarget->GetSize();
        if (drawTargetSize[0] == 0 || drawTargetSize[1] == 0) return;
        _mousePosition[0] = 2.0 * (static_cast<double>(io.MousePos.x - (window->DC.LastItemRect.Min.x)) /
            static_cast<double>(drawTargetSize[0])) -
            1.0;
        _mousePosition[1] = -2.0 * (static_cast<double>(io.MousePos.y - (window->DC.LastItemRect.Min.y)) /
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

GfCamera &Viewport::GetCurrentCamera() { return *_renderCamera; }
const GfCamera &Viewport::GetCurrentCamera() const { return *_renderCamera; }
UsdGeomCamera Viewport::GetUsdGeomCamera() { return UsdGeomCamera::Get(GetCurrentStage(), GetCameraPath()); }

void Viewport::SetCameraPath(const SdfPath &cameraPath) {
    _selectedCameraPath = cameraPath;

    _renderCamera = &_perspectiveCamera; // by default
    if (GetCurrentStage() && _renderparams) {
        const auto selectedCameraPrim = UsdGeomCamera::Get(GetCurrentStage(), _selectedCameraPath);
        if (selectedCameraPrim) {
            _renderCamera = &_stageCamera;
            _stageCamera = selectedCameraPrim.GetCamera(_renderparams->frame);
        }
    }
    GetCurrentCamera().SetPerspectiveFromAspectRatioAndFieldOfView(double(_viewportSize[0]) / double(_viewportSize[1]), 60,
                                                                   GfCamera::FOVHorizontal);
}


void Viewport::Render() {

    GfVec2i renderSize = _drawTarget->GetSize();
    int width = renderSize[0];
    int height = renderSize[1];

    if (width == 0 || height == 0) return;

    _drawTarget->Bind();
    glEnable(GL_DEPTH_TEST);
    glClearColor(_renderparams->clearColor[0], _renderparams->clearColor[1], _renderparams->clearColor[2],
        _renderparams->clearColor[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glViewport(0, 0, width, height);

    if (_renderer && GetCurrentStage()) {
        // Render hydra
        // Set camera and lighting state
        _renderer->SetLightingState(_lights, _material, _ambient);
        _renderer->SetRenderViewport(GfVec4d(0, 0, width, height));
        _renderer->SetWindowPolicy(CameraUtilConformWindowPolicy::CameraUtilMatchHorizontally);
        //_renderparams->forceRefresh = true;

        // If using a usd camera, use SetCameraPath renderer.SetCameraPath(sceneCam.GetPath())
        // else set camera state
        _renderer->SetCameraState(GetCurrentCamera().GetFrustum().ComputeViewMatrix(),
                                  GetCurrentCamera().GetFrustum().ComputeProjectionMatrix());
        _renderer->Render(GetCurrentStage()->GetPseudoRoot(), *_renderparams);
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // Draw grid
    _grid.Render(*this);

    // Draw active manipulator
    GetActiveManipulator().OnDrawFrame(*this);

    _drawTarget->Unbind();
}

void Viewport::SetCurrentTimeCode(const UsdTimeCode &tc) {
    if (_renderparams) {
        _renderparams->frame = tc;
    }
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
        } else if (whichRenderer->second != _renderer) {
            _renderer = whichRenderer->second;
            _cameraManipulator.SetZIsUp(UsdGeomGetStageUpAxis(GetCurrentStage()) == "Z");
            _grid.SetZIsUp(UsdGeomGetStageUpAxis(GetCurrentStage()) == "Z");
            // TODO: the selection is also different per stage
            //_selection =
        }
        // Camera -- TODO: is it slow to query the camera at each frame ?
        //                 the manipulator does is as well
        const auto stageCameraPrim = GetUsdGeomCamera();
        if (stageCameraPrim) {
            _stageCamera = stageCameraPrim.GetCamera(GetCurrentTimeCode());
            _renderCamera = &_stageCamera;
        }
    }

    const GfVec2i &currentSize = _drawTarget->GetSize();
    if (currentSize != _viewportSize) {
        _drawTarget->Bind();
        _drawTarget->SetSize(_viewportSize);
        _drawTarget->Unbind();
        GetCurrentCamera().SetPerspectiveFromAspectRatioAndFieldOfView(double(_viewportSize[0]) / double(_viewportSize[1]), 60,
                                                                   GfCamera::FOVHorizontal);
    }

    /// Note that the following will have terrible performances when selecting thousands of paths
    /// this is a way to check if the selection has changed since the previous frame, not the most efficient way.
    SelectionHash currentSelectionHash = GetSelectionHash(_selection);
    if (_renderer && _lastSelectionHash != currentSelectionHash) {
        _renderer->ClearSelected();
        _renderer->SetSelected(GetSelectedPaths(_selection));
        _lastSelectionHash = currentSelectionHash;

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
    return (_renderparams && _renderer &&
        _renderer->TestIntersection(GetCurrentCamera().GetFrustum().ComputeViewMatrix(),
            pixelFrustum.ComputeProjectionMatrix(),
            GetCurrentStage()->GetPseudoRoot(), *_renderparams, &outHitPoint, &outHitNormal,
            &outHitPrimPath, &outHitInstancerPath, &outHitInstanceIndex));
}
