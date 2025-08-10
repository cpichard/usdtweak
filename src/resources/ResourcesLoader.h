#pragma once
#include "EditorSettings.h"
#include "ViewportSettings.h"

// Load fonts, ini settings, texture and initialise an imgui context.
// It allows to keep data that have a longer lifetime than the editor or the widgets.
class ResourcesLoader {
  public:
    ResourcesLoader();
    ~ResourcesLoader();

    // Return the settings that the resource loader has loaded when the application starter.
    // Those settings will also be saved when the application closes.
    // Note that he editor and the viewport might have their own copy of the settings.
    static EditorSettings &GetEditorSettings();
    static ViewportSettings &GetViewportSettings();

    // The following getter/setter should ultimately move to ApplicationSettings
    static int GetApplicationWidth();
    static int GetApplicationHeight();

    // Fonts specific functions

    // Returns the paths to the used fonts.
    // The empty string is the default application embedded font
    static const std::string &GetFontRegularPath() { return _fontRegularPathLoaded; };
    static const std::string &GetFontMonoPath() { return _fontMonoPathLoaded; };
    static const std::string &GetGlyphRangeName() { return _glyphRange; };
    static const std::vector<std::string> &GetGlyphRangeNames();

    // Set the path of the desired fonts. The font won't be loaded when calling this function, for that you have to call
    // LoadFonts. This is done this way as don't want to load the fonts when parsing the config file or when drawing. The
    // resourceLoader keeps the path to the desired fonts.
    static void RequestNewFontRegular(const std::string &fontPath) { _fontRegularPathRequested = fontPath; }
    static void RequesNewFontMono(const std::string &fontPath) { _fontMonoPathRequested = fontPath; }
    static void SetGlyphRangeName(const std::string &glyphName) { _glyphRange = glyphName; }

    // Load regular and mono fonts, should be called outside of the main draw function using the command
    // EditorReloadFonts
    static void LoadFonts();

    // Push the currently load regular fonts
    static void PushFontRegular() { ImGui::PushFont(_fontRegular); }
    static void PopFontRegular() { ImGui::PopFont(); }
    static void PushFontMono() { ImGui::PushFont(_fontMono); }
    static void PopFontMono() { ImGui::PopFont(); }

    // This should not be called during a frame render.
    static void ScaleUI(float scaleValue);

  private:
    // We keep a static copy of the editor settings because the lifetime
    // of the ResourceLoader is longer than the Editor and we want locality
    // between editor and editor settings. This is not ideal and should be refactored.
    static EditorSettings _editorSettings;
    static ViewportSettings _viewportSettings;

    static std::string _fontRegularPathRequested; // What the user has asked
    static std::string _fontRegularPathLoaded;    // What is actually loaded
    static ImFont *_fontRegular;                  // Font memory location

    static std::string _fontMonoPathRequested; // What the user has asked
    static std::string _fontMonoPathLoaded;    // What is actually loaded
    static ImFont *_fontMono;                  // Font memory location

    static std::string _glyphRange;

    static bool _resourcesLoaded;
};
