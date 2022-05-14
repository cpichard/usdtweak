#pragma once
#include <list>
#include <string>
/// EditorSettings contains all the settings we want to persist between sessions

struct EditorSettings {
    /// Editor Windows state
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

    /// Recent files
    std::list<std::string> _recentFiles;
};
