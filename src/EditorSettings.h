#pragma once
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct ImGuiTextBuffer;

/// EditorSettings contains all the editor variables we want to persist between sessions
struct EditorSettings {

    EditorSettings();

    /// Editor windows states
    bool _showDebugWindow = false;
    bool _showPropertyEditor = true;
    bool _showOutliner = true;
    bool _showTimeline = false;
    bool _showLayerHierarchyEditor = false;
    bool _showLayerStackEditor = false;
    bool _showContentBrowser = false;
    bool _showPrimSpecEditor = false;
    bool _showViewport1 = false;
    bool _showViewport2 = false;
    bool _showViewport3 = false;
    bool _showViewport4 = false;
    bool _showStatusBar = true;
    bool _showValidator = false;
    bool _textEditor = false;
    bool _showTextEditorV2 = false;
    bool _showSdfAttributeEditor = false;
    bool _showUsdConnectionEditor = false;
    bool _showHydraBrowser = false;
    bool _showHydraNoticeLogger = false;
    bool _showSearch = false;
    bool _showSplashScreen = true;
    int _mainWindowWidth;
    int _mainWindowHeight;
    float _uiScale = 1.f;

    /// Last file browser directory
    std::string _lastFileBrowserDirectory;

    /// Additional plugin paths -  It really belongs to an ApplicationSettings but we want to edit it
    /// in the editor - This might move in the future
    std::vector<std::string> _pluginPaths;
    
    // Add new file to the list of recent files
    void UpdateRecentFiles(const std::string &newFile);
    const std::list<std::string> &GetRecentFiles() const { return _recentFiles; }

    
    /// Blueprints root location on disk
    std::vector<std::string> _blueprintLocations;

    // Serialization functions
    void ParseLine(const char *line);
    void Dump(ImGuiTextBuffer *);

    // Per-addon key/value bag. Persisted under "Addon.<addonId>.<key>=…".
    // Values are stored as strings; typed accessors convert on read/write.
    bool GetAddonBool(const std::string &addonId, const std::string &key, bool defaultValue) const;
    void SetAddonBool(const std::string &addonId, const std::string &key, bool value);
    std::string GetAddonString(const std::string &addonId, const std::string &key,
                               const std::string &defaultValue) const;
    void SetAddonString(const std::string &addonId, const std::string &key, const std::string &value);

  private:
    // Those are private as they rely on a specific logic, they need to be accessed
    // with their corresponding api functions

    /// Recent files
    std::list<std::string> _recentFiles;

    /// Addon key/value bag, keyed by "<addonId>.<key>".
    std::map<std::string, std::string> _addonValues;
};
