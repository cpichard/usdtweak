#pragma once
#include <list>
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
    bool _showViewport = false;
    bool _showStatusBar = true;
    bool _showLauncherBar = false;
    bool _textEditor = false;
    bool _showSdfAttributeEditor = false;
    int _mainWindowWidth;
    int _mainWindowHeight;

    /// Last file browser directory
    std::string _lastFileBrowserDirectory;

    // Add new file to the list of recent files
    void UpdateRecentFiles(const std::string &newFile);
    const std::list<std::string> &GetRecentFiles() const { return _recentFiles; }

    // Launcher commands.
    // We maintain a mapping between the commandName and the commandLine while keeping
    // the order as well. As we expect a very few number of commands, we store them
    // in 2 vectors instead of a map, it is more efficience as the code iterates on the command name list
    bool AddLauncher(const std::string &commandName, const std::string &commandLine);
    bool RemoveLauncher(const std::string &commandName);
    const std::vector<std::string> &GetLauncheNameList() const { return _launcherNames; };
    std::string GetLauncherCommandLine(const std::string &commandName) const;

    // Serialization functions
    void ParseLine(const char *line);
    void Dump(ImGuiTextBuffer *);

  private:
    /// Recent files
    std::list<std::string> _recentFiles;

    /// Launcher commands
    std::vector<std::string> _launcherNames;
    std::vector<std::string> _launcherCommandLines;
};
