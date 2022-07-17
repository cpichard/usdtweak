#pragma once
#include <list>
#include <string>

/// EditorSettings contains all the settings we want to persist between sessions
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
    bool _showViewport = false;
    bool _showStatusBar = true;
    bool _textEditor = false;
    bool _showArrayEditor = false;
    int _mainWindowWidth;
    int _mainWindowHeight;

    /// Last file browser directory
    std::string _lastFileBrowserDirectory;

    /// Recent files
    std::list<std::string> _recentFiles;
};
