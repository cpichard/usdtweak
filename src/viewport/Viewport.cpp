#include <iostream>

#include <pxr/imaging/glf/glew.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/boundable.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/imageable.h>

#include <GLFW/glfw3.h>
#define GLFW_INCLUDE_GLCOREARB
#include "Gui.h"
#include "Viewport.h"
#include "Commands.h"
#include "Constants.h"

/// Renderer settings widget, TODO: this could go in a different cpp file with specialized
/// ui for each usd components

void DrawRendererSettings(UsdImagingGLEngine &renderer, UsdImagingGLRenderParams &renderparams) {
    // General render parameters
    ImGui::ColorEdit4("Background color", renderparams.clearColor.data());

    // TODO: check there isn't a function that already returns the strings for an enum
    // I have seen that in the USD code
    static std::array<std::pair<UsdImagingGLDrawMode, const char *>, 8> UsdImagingGLDrawModeStrings = {
        std::make_pair(UsdImagingGLDrawMode::DRAW_POINTS, "DRAW_POINTS"),
        std::make_pair(UsdImagingGLDrawMode::DRAW_WIREFRAME, "DRAW_WIREFRAME"),
        std::make_pair(UsdImagingGLDrawMode::DRAW_WIREFRAME_ON_SURFACE, "DRAW_WIREFRAME_ON_SURFACE"),
        std::make_pair(UsdImagingGLDrawMode::DRAW_SHADED_FLAT, "DRAW_SHADED_FLAT"),
        std::make_pair(UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH, "DRAW_SHADED_SMOOTH"),
        std::make_pair(UsdImagingGLDrawMode::DRAW_GEOM_ONLY, "DRAW_GEOM_ONLY"),
        std::make_pair(UsdImagingGLDrawMode::DRAW_GEOM_FLAT, "DRAW_GEOM_FLAT"),
        std::make_pair(UsdImagingGLDrawMode::DRAW_GEOM_SMOOTH, "DRAW_GEOM_SMOOTH")};

    // Look for the current on
    auto currentdrawmode = UsdImagingGLDrawModeStrings[0];
    for (const auto &mode : UsdImagingGLDrawModeStrings) {
        if (renderparams.drawMode == mode.first) {
            currentdrawmode = mode;
        }
    }
    if (ImGui::BeginCombo("Draw mode", currentdrawmode.second)) {
        for (const auto &drawMode : UsdImagingGLDrawModeStrings) {
            bool is_selected = (renderparams.drawMode == drawMode.first);
            if (ImGui::Selectable(drawMode.second, is_selected)) {
                renderparams.drawMode = drawMode.first;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Checkbox("Enable lighting", &renderparams.enableLighting);
    ImGui::Checkbox("Enable scene materials", &renderparams.enableSceneMaterials);
    ImGui::Checkbox("Enable ID render", &renderparams.enableIdRender);

    // Renderer
    ImGui::Separator();
    const auto currentPlugin = renderer.GetCurrentRendererId();
    if (ImGui::BeginCombo("Renderer", currentPlugin.GetText())) {
        auto plugins = renderer.GetRendererPlugins();
        for (int n = 0; n < plugins.size(); n++) {
            bool is_selected = (currentPlugin == plugins[n]);
            if (ImGui::Selectable(plugins[n].GetText(), is_selected)) {
                if (!renderer.SetRendererPlugin(plugins[n])) {
                    std::cerr << "unable to set default renderer plugin" << std::endl;
                }
            }
            if (is_selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Renderer settings
    for (auto setting : renderer.GetRendererSettingsList()) {
        // TODO: create a dedicated interface for each parameter
        ImGui::Text("%s", setting.name.c_str());
    }
}

template <typename BasicShader> void DrawBasicShadingProperties(BasicShader &shader) {
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
            ImGui::InputFloat4("position", position.data());
            light.SetPosition(position);
            DrawBasicShadingProperties(light);
            ImGui::TreePop();
        }
    }
}

/// Draw a camera selection
void DrawCameraList(Viewport &viewport) {
    // TODO: the viewport cameras and the stage camera should live in different lists
    constexpr char const *freeCamera = "Perspective";
    if (ImGui::ListBoxHeader("")) {
        /// TODO
        // for (auto cam : viewport.cameras) {
        //    // Camera provided by the applications

        //}

        if (ImGui::Selectable(freeCamera, viewport._selectedCameraPath == SdfPath::EmptyPath())) {
            viewport._selectedCameraPath = SdfPath::EmptyPath();
        }
        if (viewport.GetCurrentStage()) {
            UsdPrimRange range = viewport.GetCurrentStage()->Traverse();
            for (const auto &prim : range) {
                if (prim.IsA<UsdGeomCamera>()) {
                    const bool is_selected = (prim.GetPath() == viewport._selectedCameraPath);
                    if (ImGui::Selectable(prim.GetName().data(), is_selected)) {
                        viewport._selectedCameraPath = prim.GetPath();
                    }
                }
            }
        }
        ImGui::ListBoxFooter();
    }
}

/// Draw the viewport wi
void Viewport::Draw() {
    ImVec2 wsize = ImGui::GetWindowSize();
    ImGui::Button("Cameras");
    ImGuiPopupFlags flags = ImGuiPopupFlags_MouseButtonLeft;
    if (ImGui::BeginPopupContextItem("Cameras", flags) && _renderer) {
        DrawCameraList(*this);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Button("Lighting");
    if (ImGui::BeginPopupContextItem("Lighting", flags) && _renderer) {
        ImGui::BulletText("Default shader");
        DrawBasicShadingProperties(_material);
        ImGui::Separator();
        ImGui::BulletText("Ambient light");
        ImGui::InputFloat4("Ambient", _ambient.data());
        ImGui::Separator();
        if (ImGui::TreeNode("Lights")) {
            DrawGLLights(_lights);
            ImGui::TreePop();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Button("Render settings");
    if (ImGui::BeginPopupContextItem("Render settings", flags) && _renderer) {
        DrawRendererSettings(*_renderer, *_renderparams);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame Stage")) {
        FrameRootPrim();
    }

    if (_textureId) {
        // Get the size of the child (i.e. the whole draw size of the windows).
        ImGui::Image((ImTextureID)_textureId, ImVec2(wsize.x, wsize.y - ViewportBorderSize), ImVec2(0, 1), ImVec2(1, 0));
    }
}

Viewport::Viewport(UsdStageRefPtr stage) : _stage(stage),
_cameraManipulator({InitialWindowWidth, InitialWindowHeight})
 {
    _handleEventFunction = &Viewport::HandleHovering;

    // Viewport draw target
    _cameraManipulator.Reset(_currentCamera);
    _drawTarget = GlfDrawTarget::New(GfVec2i(InitialWindowWidth, InitialWindowHeight), false);
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
    _renderparams->showRender = false;   // what is it used for ?
    _renderparams->forceRefresh = false; // what is it used for ?
    _renderparams->enableLighting = true;
    _renderparams->enableSceneMaterials = true;
    _renderparams->drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
    _renderparams->highlight = true;

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
        renderer.second->InvalidateBuffers();
        delete renderer.second;
        renderer.second = nullptr;
    }
    _drawTarget->Unbind();
    _renderers.clear();

    if (_renderparams) {
        delete _renderparams;
        _renderparams = nullptr;
    }
}

/// Resize the Hydra viewport/render panel
void Viewport::SetSize(int width, int height) {
    const GfVec2i &currentSize = _drawTarget->GetSize();
    if (currentSize != GfVec2i(width, height)) {
        _drawTarget->Bind();
        _drawTarget->SetSize(GfVec2i(width, height));
        _drawTarget->Unbind();
        _currentCamera.SetPerspectiveFromAspectRatioAndFieldOfView(double(width) / double(height), 60,
                                                                  GfCamera::FOVHorizontal);
    }
}

/// Frane the viewport using the bounding box of the selection
void Viewport::FrameSelection(const Selection &selection) {
    if (GetCurrentStage() && selection) {
        UsdGeomBBoxCache bboxcache(_renderparams->frame, UsdGeomImageable::GetOrderedPurposeTokens());
        GfBBox3d bbox;
        for (const auto &primPath : selection->GetAllSelectedPrimPaths()) {
            bbox = GfBBox3d::Combine(bboxcache.ComputeWorldBound(GetCurrentStage()->GetPrimAtPath(primPath)), bbox);
        }
        auto defaultPrim = GetCurrentStage()->GetDefaultPrim();
        _cameraManipulator.FrameBoundingBox(_currentCamera, bbox);
    }
}

/// Frane the viewport using the bounding box of the root prim
void Viewport::FrameRootPrim(){
    if (GetCurrentStage()) {
        UsdGeomBBoxCache bboxcache(_renderparams->frame, UsdGeomImageable::GetOrderedPurposeTokens());
        auto defaultPrim = GetCurrentStage()->GetDefaultPrim();
        _cameraManipulator.FrameBoundingBox(_currentCamera, bboxcache.ComputeWorldBound(defaultPrim));
    }
}


void Viewport::HandleHovering(Selection & selection) {
    ImGuiIO &io = ImGui::GetIO();
    if (io.KeysDown[GLFW_KEY_LEFT_ALT]) {
        // Changing the calling function ... does that work really ???
        _handleEventFunction = &Viewport::HandleCameraEditing;
        return;
    }
    else if (ImGui::IsMouseClicked(0)) {
        // Left click start picking !
        _handleEventFunction = &Viewport::HandleSelecting;
        return;
    }
    else if (ImGui::IsKeyPressed(GLFW_KEY_F)) {
        if (GetCurrentStage() && _renderparams) {
            FrameSelection(selection);
        }
    }
}


void Viewport::HandleCameraEditing(Selection & selection) {

    ImGuiIO &io = ImGui::GetIO();

    /// If the user released key alt, escape camera manipulation
    if (!io.KeysDown[GLFW_KEY_LEFT_ALT]) {
        _handleEventFunction = &Viewport::HandleHovering;
        return;
    }
    else if (ImGui::IsMouseReleased(1) || ImGui::IsMouseReleased(2) || ImGui::IsMouseReleased(0)) {
        _cameraManipulator.SetMovementType(MovementType::None);
    }
    else if (ImGui::IsMouseClicked(0)) {
        _cameraManipulator.SetMovementType(MovementType::Orbit);
    }
    else if (ImGui::IsMouseClicked(2)) {
        _cameraManipulator.SetMovementType(MovementType::Truck);
    }
    else if (ImGui::IsMouseClicked(1)) {
        _cameraManipulator.SetMovementType(MovementType::Dolly);
    }

    _cameraManipulator.Move(_currentCamera, io.MouseDelta.x, io.MouseDelta.y);
}


void Viewport::HandleManipulatorEditing(Selection & selection) {
    // The selected manipulator captures events
    auto mousePosition = GetMousePosition();
    if (!_translateManipulator.CaptureEvents(_currentCamera, mousePosition)) {
        _handleEventFunction = &Viewport::HandleHovering;
    }
}

/// Called when there was a left click
void Viewport::HandleSelecting(Selection & selection) {
    GfVec2i renderSize = _drawTarget->GetSize();
    double width = static_cast<double>(renderSize[0]);
    double height = static_cast<double>(renderSize[1]);
    const GfVec2d clickedPoint(_mouseX, _mouseY);
    const GfVec2d limits(20.0 / width, 20.0 / height);

    // Escape this state by releasing the mouse
    if (ImGui::IsMouseReleased(0)) {
        _handleEventFunction = &Viewport::HandleHovering;
        return;
    }

    /// What is under the mouse ? start looking at the manipulators
    if( _translateManipulator.Pick(_currentCamera.GetFrustum().ComputeViewMatrix(),
                _currentCamera.GetFrustum().ComputeProjectionMatrix(), clickedPoint,
                limits)) {
                _handleEventFunction = &Viewport::HandleManipulatorEditing;
                // TODO this should also select the manipulator
                // _selectedManipulator = _translateManipulator;
    }
    // Then look at the objects in the scene
    else {

        // Should that go in a SelectionManipulator ????
        GfFrustum pixelFrustum = _currentCamera.GetFrustum().ComputeNarrowedFrustum(
            clickedPoint, GfVec2d(1.0 / width, 1.0 / height));
        // Make a pixel frustum
        GfVec3d outHitPoint;
        SdfPath outHitPrimPath;
        SdfPath outHitInstancerPath;
        int outHitInstanceIndex = 0;

        if (_renderparams &&
            _renderer->TestIntersection(_currentCamera.GetFrustum().ComputeViewMatrix(),
                pixelFrustum.ComputeProjectionMatrix(),
                GetCurrentStage()->GetPseudoRoot(), *_renderparams, &outHitPoint,
                &outHitPrimPath, &outHitInstancerPath, &outHitInstanceIndex)) {
            // Add to selection
            if (!outHitPrimPath.IsEmpty()) {
                if (ImGui::IsKeyDown(GLFW_KEY_LEFT_SHIFT)) {
                    AddSelection(selection, outHitPrimPath);
                }
                else {
                    SetSelected(selection, outHitPrimPath);
                }
            }
            else if (!outHitInstancerPath.IsEmpty()) {
                /// TODO: manage selection
                ClearSelection(selection);
            }
        }
        else {
            ClearSelection(selection);
        }
    }
}

void Viewport::HandleEvents(Selection & selection) {

    ImGuiContext *g = ImGui::GetCurrentContext();
    ImGuiWindow *window = g->CurrentWindow;
    ImGuiIO &io = ImGui::GetIO();

    if (ImGui::IsItemHovered()) {
        const GfVec2i drawTargetSize = _drawTarget->GetSize();
        if (drawTargetSize[0] == 0 || drawTargetSize[1] == 0) return;
        _mouseX = 2.0 * (static_cast<double>(io.MousePos.x - (window->DC.LastItemRect.Min.x)) /
            static_cast<double>(drawTargetSize[0])) -
            1.0;
        _mouseY = -2.0 * (static_cast<double>(io.MousePos.y - (window->DC.LastItemRect.Min.y)) /
            static_cast<double>(drawTargetSize[1])) +
            1.0;
        _handleEventFunction(*this, selection);
    }
}

void Viewport::Render(const Selection & selection) {

    GfVec2i renderSize = _drawTarget->GetSize();
    int width = renderSize[0];
    int height = renderSize[1];

    if (width == 0 || height == 0) return;

    /// Use the selected camera
    if (GetCurrentStage() && _renderparams && _selectedCameraPath != SdfPath::EmptyPath()) {
        auto selectedCameraPrim = UsdGeomCamera::Get(GetCurrentStage(), _selectedCameraPath);
        // Overrides and destroy the freeCamera position,
        _currentCamera = selectedCameraPrim.GetCamera(_renderparams->frame);
        // cameraManipulator.CopyCameraAtPath(freeCamera, selectedCameraPath);
    }

    // Set camera and lighting state
    _drawTarget->Bind();
    glEnable(GL_DEPTH_TEST);
    glClearColor(_renderparams->clearColor[0], _renderparams->clearColor[1], _renderparams->clearColor[2],
        _renderparams->clearColor[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (_renderer && GetCurrentStage()) {
        //
        glViewport(0, 0, width, height);
        _renderer->SetLightingState(_lights, _material, _ambient);

        // Render hydra
        _renderer->SetRenderViewport(GfVec4d(0, 0, width, height));
        _renderer->SetWindowPolicy(CameraUtilConformWindowPolicy::CameraUtilMatchHorizontally);
        _renderparams->forceRefresh = true;

        // If using a usd camera, use SetCameraPath renderer.SetCameraPath(sceneCam.GetPath())
        // else set camera state
        _renderer->SetCameraState(_currentCamera.GetFrustum().ComputeViewMatrix(),
            _currentCamera.GetFrustum().ComputeProjectionMatrix());
        _renderer->Render(GetCurrentStage()->GetPseudoRoot(), *_renderparams);
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    //// Draw Manipulators.. need to write a loop on the manipulators in the scene
    _translateManipulator.Draw(_currentCamera.GetFrustum().ComputeViewMatrix(),
        _currentCamera.GetFrustum().ComputeProjectionMatrix());

    _drawTarget->Unbind();
}

/// Check if the current stage has a renderer associated to it
/// Create one if needed
void Viewport::Update(const Selection &selection) {
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
        }
        else if (whichRenderer->second != _renderer) {
            _renderer = whichRenderer->second;
            _cameraManipulator.SetZIsUp(UsdGeomGetStageUpAxis(GetCurrentStage()) == "Z");
        }
    }

    /// Note that the following will have terrible performances when selecting thousands of paths
    /// this is a way to check if the selection has changed since the previous frame, not the most efficient way.
    SelectionHash currentSelectionHash = GetSelectionHash(selection);
    if (_renderer && _lastSelectionHash != currentSelectionHash) {
        _renderer->ClearSelected();
        _renderer->SetSelected(GetSelectedPaths(selection));
        _lastSelectionHash = currentSelectionHash;
    }
}
