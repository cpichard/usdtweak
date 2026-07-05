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

    // Persist the current settings (including addon settings) to the config file
    // immediately. Settings are otherwise only written on a clean shutdown (see
    // the destructor), so anything that must survive a crash/kill should call
    // this right after mutating a setting.
    static void SaveSettings();

    // The following getter/setter should ultimately move to ApplicationSettings
    static int GetApplicationWidth();
    static int GetApplicationHeight();

    // Fonts specific functions

    // Returns the paths to the used fonts.
    // The empty string is the default application embedded font
    static const std::string &GetFontRegularPath() { return _fontRegularPathLoaded; };
    static const std::string &GetFontMonoPath() { return _fontMonoPathLoaded; };
    static const std::string &GetFontBoldPath() { return _fontBoldPathLoaded; };
    static const std::string &GetFontItalicPath() { return _fontItalicPathLoaded; };
    static const std::string &GetGlyphRangeName() { return _glyphRange; };
    static const std::vector<std::string> &GetGlyphRangeNames();

    // Set the path of the desired fonts. The font won't be loaded when calling this function, for that you have to call
    // LoadFonts. This is done this way as don't want to load the fonts when parsing the config file or when drawing. The
    // resourceLoader keeps the path to the desired fonts.
    static void RequestNewFontRegular(const std::string &fontPath) { _fontRegularPathRequested = fontPath; }
    static void RequesNewFontMono(const std::string &fontPath) { _fontMonoPathRequested = fontPath; }
    static void RequestNewFontBold(const std::string &fontPath) { _fontBoldPathRequested = fontPath; }
    static void RequestNewFontItalic(const std::string &fontPath) { _fontItalicPathRequested = fontPath; }
    static void SetGlyphRangeName(const std::string &glyphName) { _glyphRange = glyphName; }

    // Load regular and mono fonts, should be called outside of the main draw function using the command
    // EditorReloadFonts
    static void LoadFonts();

    // Push the currently loaded fonts
    static void PushFontRegular() { ImGui::PushFont(_fontRegular); }
    static void PopFontRegular() { ImGui::PopFont(); }
    static void PushFontMono() { ImGui::PushFont(_fontMono); }
    static void PopFontMono() { ImGui::PopFont(); }
    static void PushFontBold() { ImGui::PushFont(_fontBold ? _fontBold : _fontRegular); }
    static void PopFontBold() { ImGui::PopFont(); }
    static void PushFontItalic() { ImGui::PushFont(_fontItalic ? _fontItalic : _fontRegular); }
    static void PopFontItalic() { ImGui::PopFont(); }

    // Raw font pointers — needed to fill ImGui::MarkdownConfig::headingFormats.
    // Fall back to _fontRegular when the bold/italic slot is not loaded yet.
    static ImFont* GetFontBoldPtr()   { return _fontBold   ? _fontBold   : _fontRegular; }
    static ImFont* GetFontItalicPtr() { return _fontItalic ? _fontItalic : _fontRegular; }
    static ImFont* GetFontMonoPtr()   { return _fontMono; }

    // This should not be called during a frame render.
    static void ScaleUI(float scaleValue);

  private:
    // We keep a static copy of the editor settings because the lifetime
    // of the ResourceLoader is longer than the Editor and we want locality
    // between editor and editor settings. This is not ideal and should be refactored.
    static EditorSettings _editorSettings;
    static ViewportSettings _viewportSettings;

    static std::string _fontRegularPathRequested;
    static std::string _fontRegularPathLoaded;
    static ImFont *_fontRegular;

    static std::string _fontMonoPathRequested;
    static std::string _fontMonoPathLoaded;
    static ImFont *_fontMono;

    static std::string _fontBoldPathRequested;
    static std::string _fontBoldPathLoaded;
    static ImFont *_fontBold;

    static std::string _fontItalicPathRequested;
    static std::string _fontItalicPathLoaded;
    static ImFont *_fontItalic;

    static std::string _glyphRange;

    static bool _resourcesLoaded;
};
