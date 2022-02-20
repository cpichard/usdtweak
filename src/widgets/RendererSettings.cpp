#include <iostream>
#include <map>
#include "RendererSettings.h"
#include "ValueEditor.h"
#include "Gui.h"

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

void DrawOpenGLSettings(UsdImagingGLEngine &renderer, UsdImagingGLRenderParams &renderparams) {
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
}

void DrawRendererSettings(UsdImagingGLEngine &renderer, UsdImagingGLRenderParams &renderparams) {
    // Renderer
    const auto currentPlugin = renderer.GetCurrentRendererId();
    if (ImGui::BeginCombo("Renderer", currentPlugin.GetText())) {
        auto plugins = renderer.GetRendererPlugins();
        for (int n = 0; n < plugins.size(); n++) {
            bool is_selected = (currentPlugin == plugins[n]);
            if (ImGui::Selectable(plugins[n].GetText(), is_selected)) {
                if (!renderer.SetRendererPlugin(plugins[n])) {
                    std::cerr << "unable to set default renderer plugin" << std::endl;
                } else {
                    renderer.SetRendererAov(GetAovSelection(renderer));
                }
            }
            if (is_selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Renderer settings
    for (auto setting : renderer.GetRendererSettingsList()) {
        VtValue currentValue = renderer.GetRendererSetting(setting.key);
        VtValue newValue = DrawVtValue(setting.name, currentValue);
        if (newValue != VtValue()) {
            renderer.SetRendererSetting(setting.key, newValue);
        }
    }

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

void DrawColorCorrection(UsdImagingGLEngine &renderer, UsdImagingGLRenderParams &renderparams) {

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