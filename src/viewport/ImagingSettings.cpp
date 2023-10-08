#include <iostream>
#include <map>
#include "ImagingSettings.h"
#include "VtValueEditor.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "Constants.h"


template <typename HasPositionT> inline void CopyCameraPosition(const GfCamera &camera, HasPositionT &object) {
    GfVec3d camPos = camera.GetFrustum().GetPosition();
    GfVec4f lightPos(camPos[0], camPos[1], camPos[2], 1.0);
    object.SetPosition(lightPos);
}

void ImagingSettings::SetLightPositionFromCamera(const GfCamera &camera) {
    if (_lights.empty())
        return;
    CopyCameraPosition(camera, _lights[0]);
}

ImagingSettings::ImagingSettings() {
    frame = 1.0;
    complexity = 1.0;
    clearColor = GfVec4f(0.12, 0.12, 0.12, 1.0);
    showRender = false;
    forceRefresh = false;
    enableLighting = true;
    enableSceneMaterials = false;
    drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
    highlight = true;
    gammaCorrectColors = false;
    colorCorrectionMode = TfToken("sRGB");
    showGuides = true;
    showProxy = true;
    showRender = false;

    // Lights
    enableCameraLight = true;

    showGrid = true;

    // TODO: set color correction as well

    // Default material
    _material.SetAmbient({0.0, 0.0, 0.0, 1.f});
    _material.SetDiffuse({1.0, 1.0, 1.0, 1.f});
    _material.SetSpecular({0.2, 0.2, 0.2, 1.f});
    _ambient = {0.0, 0.0, 0.0, 0.0};
}

const GlfSimpleLightVector &ImagingSettings::GetLights() {
    // Only a camera light for the moment
    if (enableCameraLight && _lights.empty()) {
        GlfSimpleLight simpleLight;
        simpleLight.SetAmbient({0.2, 0.2, 0.2, 1.0});
        simpleLight.SetDiffuse({1.0, 1.0, 1.0, 1.f});
        simpleLight.SetSpecular({0.2, 0.2, 0.2, 1.f});
        simpleLight.SetPosition({200, 200, 200, 1.0});
        _lights.push_back(simpleLight);
    }
    if (!enableCameraLight && !_lights.empty()) {
        _lights.clear();
    }
    return _lights;
}


// We keep the currently selected AOV per engine here as there is it not really store in UsdImagingGLEngine.
// When setting a color aov, the engine adds multiple other aov to render, in short there is no easy way to know
// which aov is rendered.
// This is obviously not thread safe but supposed to work in the main rendering thread
static std::map<TfToken, TfToken> aovSelection; // a vector might be faster

static void SetAovSelection(UsdImagingGLEngine &renderer, TfToken aov) { aovSelection[renderer.GetCurrentRendererId()] = aov; }

static TfToken GetAovSelection(UsdImagingGLEngine &renderer) {
    auto aov = aovSelection.find(renderer.GetCurrentRendererId());
    if (aov == aovSelection.end()) {
        SetAovSelection(renderer, TfToken("color"));
        return TfToken("color");
    } else {
        return aov->second;
    }
}

void InitializeRendererAov(UsdImagingGLEngine &renderer) {
    renderer.SetRendererAov(GetAovSelection(renderer));
}

void DrawImagingSettings(UsdImagingGLEngine &renderer, ImagingSettings &renderparams) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
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
    ImGui::SliderFloat("Complexity", &renderparams.complexity, 1.0f, 1.4f, "%.1f");
    ImGui::Separator();
    if (ImGui::BeginMenu("Show purpose")) {
        ImGui::Checkbox("guides", &renderparams.showGuides);
        ImGui::Checkbox("proxy", &renderparams.showProxy);
        ImGui::Checkbox("render", &renderparams.showRender);
        ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::Checkbox("Highlight selection", &renderparams.highlight);

    ImGui::Separator();
    ImGui::Checkbox("Enable lighting", &renderparams.enableLighting);
    ImGui::Checkbox("Enable scene materials", &renderparams.enableSceneMaterials);
    ImGui::Checkbox("Enable scene lights", &renderparams.enableSceneLights);
    ImGui::Checkbox("Enable ID render", &renderparams.enableIdRender);
    ImGui::Checkbox("Enable USD draw modes", &renderparams.enableUsdDrawModes);
    ImGui::Checkbox("Enable camera light", &renderparams.enableCameraLight);
    ImGui::Checkbox("Show grid", &renderparams.showGrid);
}

void DrawRendererSelectionCombo(UsdImagingGLEngine &renderer) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    const auto currentPlugin = renderer.GetCurrentRendererId();
    std::string pluginName = renderer.GetRendererDisplayName(currentPlugin);
    if (ImGui::BeginCombo("Renderer", pluginName.c_str())) {
        DrawRendererSelectionList(renderer);
        ImGui::EndCombo();
    }
}

void DrawRendererSelectionList(UsdImagingGLEngine &renderer) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    const auto currentPlugin = renderer.GetCurrentRendererId();
    auto plugins = renderer.GetRendererPlugins();
    for (int n = 0; n < plugins.size(); n++) {
        bool is_selected = (currentPlugin == plugins[n]);
        std::string pluginName = renderer.GetRendererDisplayName(plugins[n]);
        if (ImGui::Selectable(pluginName.c_str(), is_selected)) {
            // TODO: changing the plugin while metal is still processing will error and crash the app.
            // We could create an ExecuteAferDraw command to defer the change of the plugin
            // after the drawing but there is no certainty as if it will solve the issue.
            if (!renderer.SetRendererPlugin(plugins[n])) {
                std::cerr << "unable to set default renderer plugin" << std::endl;
            } else {
                renderer.SetRendererAov(GetAovSelection(renderer));
            }
        }
        if (is_selected)
            ImGui::SetItemDefaultFocus();
    }

}

void DrawRendererControls(UsdImagingGLEngine &renderer) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    if (renderer.IsPauseRendererSupported()) {
        ImGui::Separator();
        if (ImGui::Button("Pause")) {
            renderer.PauseRenderer();
        }
        ImGui::SameLine();
        if (ImGui::Button("Resume")) {
            renderer.ResumeRenderer();
        }
    }
    if (renderer.IsStopRendererSupported()) {
        if (ImGui::Button("Stop")) {
            renderer.StopRenderer();
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart")) {
            renderer.RestartRenderer();
        }
    }
}

void DrawRendererCommands(UsdImagingGLEngine &renderer) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    if (ImGui::BeginMenu("Renderer Commands")) {
        HdCommandDescriptors commands = renderer.GetRendererCommandDescriptors();
        for (const auto &command : commands) {
            if (ImGui::Selectable(command.commandDescription.c_str())) {
                // TODO: that should be executed after drawing the UI
                // the following code should live in a command
                renderer.InvokeRendererCommand(command.commandName);
            }
        }
        ImGui::EndMenu();
    }
}


void DrawRendererSettings(UsdImagingGLEngine &renderer, ImagingSettings &renderparams) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    // Renderer settings
    for (auto setting : renderer.GetRendererSettingsList()) {
        VtValue currentValue = renderer.GetRendererSetting(setting.key);
        VtValue newValue = DrawVtValue(setting.name, currentValue);
        if (newValue != VtValue()) {
            // TODO: that should be executed after drawing the UI
            // the following code should live in a command
            renderer.SetRendererSetting(setting.key, newValue);
        }
    }
}

void DrawColorCorrection(UsdImagingGLEngine &renderer, ImagingSettings &renderparams) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    if (renderer.IsColorCorrectionCapable() && GetAovSelection(renderer) == TfToken("color")) {
        ImGui::Separator();
        // Gamma correction is not implemented yet in usd
        // ImGui::Checkbox("Gamma correction", &renderparams.gammaCorrectColors);
        if (ImGui::BeginCombo("Color correction mode", renderparams.colorCorrectionMode.GetText())) {
            if (ImGui::Selectable("disabled")) {
                renderparams.colorCorrectionMode = TfToken("disabled");
            }
            if (ImGui::Selectable("sRGB")) {
                renderparams.colorCorrectionMode = TfToken("sRGB");
            }
            if (ImGui::Selectable("openColorIO")) {
                renderparams.colorCorrectionMode = TfToken("openColorIO");
            }
            ImGui::EndCombo();
        }
        // TODO: opencolorIO configuration
    }
}

void DrawAovSettings(UsdImagingGLEngine &renderer) {
    ScopedStyleColor defaultStyle(DefaultColorStyle);
    TfToken newSelection;
    const TfToken selectedAov = GetAovSelection(renderer);
    if (ImGui::BeginCombo("AOV", selectedAov.GetString().c_str())) {
        for (auto &aov : renderer.GetRendererAovs()) {
            if (ImGui::Selectable(aov.GetString().c_str())) {
                newSelection = aov;
            }
        }
        ImGui::EndCombo();
    }
    if (newSelection != TfToken()) {
        renderer.SetRendererAov(newSelection);
        SetAovSelection(renderer, newSelection);
    }
}
