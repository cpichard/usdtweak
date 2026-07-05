#include "Preferences.h"
#include "Commands.h"
#include "Editor.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "ResourcesLoader.h"
#include <Style.h>
#include <iostream>

PreferencesModalDialog::PreferencesModalDialog(Editor &editor) : editor(editor) {};

void PreferencesModalDialog::Draw() {
    static const char *const panels[] = {"General", "Viewport", "Style", "Fonts", "Experimental"};
    static int current_item = 0;
    const ImGuiContext &g = *GImGui;

    const float heightWithoutCloseButton = RemainingHeight(1);
    ImVec2 prefContentSize(0, heightWithoutCloseButton);
    ImVec2 prefTabSize(g.FontSize * 5, heightWithoutCloseButton);
    if (ImGui::BeginListBox("##PreferencePanels", prefTabSize)) {
        for (int i = 0; i < 5; ++i) {
            if (ImGui::Selectable(panels[i], i == current_item)) {
                current_item = i;
            }
        }
        ImGui::EndListBox();
    }

    ImGui::SameLine();
    if (current_item == 0) {
        if (ImGui::BeginChild("##General", prefContentSize)) {
            float uiScale = editor.GetUIScale();
            if (ImGui::SliderFloat("UI scaling", &uiScale, 0.5f, 3.0f, "%.2f", ImGuiSliderFlags_NoRoundToFormat)) {
                ExecuteAfterDraw<EditorScaleUI>(uiScale);
            }
            if (ImGui::Button("Reset scaling")) {
                ExecuteAfterDraw<EditorScaleUI>(1.f);
            }
            ImGui::Separator();
            ImGui::Checkbox("Show splash screen at startup", &editor.GetShowSplashScreen());
            ImGui::EndChild();
        }
    } else if (current_item == 1) {
        if (ImGui::BeginChild("##Viewport", prefContentSize)) {
            ViewportSettings &viewportSettings = ResourcesLoader::GetViewportSettings();
            ImGui::Checkbox("Texture On by default", &viewportSettings._useMaterials);
            ImGui::Checkbox("Snap playback to whole frames", &viewportSettings._snapPlaybackToFrame);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("On (default): playback lands on whole frames, keeping topology-varying\n"
                                  "meshes coherent. Off: fractional/subframe timecodes are sent to Hydra so\n"
                                  "motion blur can be introspected between frames.");
            }
            ImGui::EndChild();
        }

    } else if (current_item == 2) {
        if (ImGui::BeginChild("##Style", prefContentSize)) {
            ShowStyleEditor(nullptr);
            ImGui::EndChild();
        }
    } else if (current_item == 3) {
        if (ImGui::BeginChild("##Fonts", prefContentSize)) {
            ImGui::Text("Select font from your operating system:");
            std::string font = ResourcesLoader::GetFontRegularPath();
            std::string fontMono = ResourcesLoader::GetFontMonoPath();
            std::string fontBold = ResourcesLoader::GetFontBoldPath();
            std::string fontItalic = ResourcesLoader::GetFontItalicPath();
            ImGui::InputTextWithHint("Regular Font", "Default: IBM Plex Sans Medium", &font);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ResourcesLoader::RequestNewFontRegular(font);
                ExecuteAfterDraw<EditorReloadFonts>();
            }

            ImGui::InputTextWithHint("Bold Font", "Default: IBM Plex Sans Bold", &fontBold);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ResourcesLoader::RequestNewFontBold(fontBold);
                ExecuteAfterDraw<EditorReloadFonts>();
            }

            ImGui::InputTextWithHint("Italic Font", "Default: IBM Plex Sans Italic", &fontItalic);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ResourcesLoader::RequestNewFontItalic(fontItalic);
                ExecuteAfterDraw<EditorReloadFonts>();
            }

            ImGui::InputTextWithHint("Mono Font", "Default: IBM Plex Mono", &fontMono);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                ResourcesLoader::RequesNewFontMono(fontMono);
                ExecuteAfterDraw<EditorReloadFonts>();
            }

            std::string selectedGlyph = ResourcesLoader::GetGlyphRangeName();
            if (ImGui::BeginCombo("Glyph range", selectedGlyph.c_str())) {
                for (const std::string &glyphRangeName : ResourcesLoader::GetGlyphRangeNames()) {
                    if (ImGui::Selectable(glyphRangeName.c_str())) {
                        ResourcesLoader::SetGlyphRangeName(glyphRangeName);
                        ExecuteAfterDraw<EditorReloadFonts>();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Separator();
            ImGui::Text("Selecting a unicode fonts allow to display a wider range of characters, chinese, japanese, etc.");
            ImGui::Text("You also have to select the glyph range according to the set of glyphs you want to display.");
            ImGui::Text("On the downside, having extended fonts will use more graphic memory and result in a slower startup.");
            ImGui::NewLine();
#ifdef __APPLE__
            ImGui::Text("On macOS, a potential font could be: ");
            ImGui::Text("/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
            if (ImGui::Button("Try Unicode fonts")) {
                ResourcesLoader::RequestNewFontRegular("/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
                ResourcesLoader::RequesNewFontMono("/System/Library/Fonts/Supplemental/Courier New.ttf");
                ExecuteAfterDraw<EditorReloadFonts>();
            }
            ImGui::SameLine();
#endif
#ifdef _WIN64
            ImGui::Text("On windows, a potential font could be: ");
            ImGui::Text("C:\\Windows\\Fonts\\ARIALUNI.TTF");
            if (ImGui::Button("Try Unicode font")) {
                ResourcesLoader::RequestNewFontRegular("C:\\Windows\\Fonts\\ARIALUNI.TTF");
                ExecuteAfterDraw<EditorReloadFonts>();
            }
            ImGui::SameLine();
#endif
            if (ImGui::Button("Reset to default fonts")) {
                if (!font.empty())
                    ResourcesLoader::RequestNewFontRegular("");
                if (!fontMono.empty())
                    ResourcesLoader::RequesNewFontMono("");
                if (!fontBold.empty())
                    ResourcesLoader::RequestNewFontBold("");
                if (!fontItalic.empty())
                    ResourcesLoader::RequestNewFontItalic("");
                if (selectedGlyph != "Default")
                    ResourcesLoader::SetGlyphRangeName("");
                ExecuteAfterDraw<EditorReloadFonts>();
            }
            ImGui::EndChild();
        }
    } else if (current_item == 4) {
        if (ImGui::BeginChild("##Experimental", prefContentSize)) {
            ImGui::Checkbox("Mouse Capture in Viewport and Connection Editor", &editor.GetEnableMouseCapture());
            ImGui::EndChild();
        }
    }
    DrawModalButtonClose();
}
