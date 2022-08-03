#include "Constants.h"
#include "EditorSettings.h"

#include <algorithm>

#include <imgui.h> // for ImGuiTextBuffer

EditorSettings::EditorSettings() : _mainWindowWidth(InitialWindowWidth), _mainWindowHeight(InitialWindowHeight) {}

void EditorSettings::ParseLine(const char *line) {
    int value = 0;
    char strBuffer[1024];
    strBuffer[0] = 0;
    if (sscanf(line, "ShowLayerEditor=%i", &value) == 1) {
        // Discarding old preference
    } else if (sscanf(line, "ShowLayerHierarchyEditor=%i", &value) == 1) {
        _showLayerHierarchyEditor = static_cast<bool>(value);
    } else if (sscanf(line, "ShowLayerStackEditor=%i", &value) == 1) {
        _showLayerStackEditor = static_cast<bool>(value);
    } else if (sscanf(line, "ShowPropertyEditor=%i", &value) == 1) {
        _showPropertyEditor = static_cast<bool>(value);
    } else if (sscanf(line, "ShowOutliner=%i", &value) == 1) {
        _showOutliner = static_cast<bool>(value);
    } else if (sscanf(line, "ShowTimeline=%i", &value) == 1) {
        _showTimeline = static_cast<bool>(value);
    } else if (sscanf(line, "ShowContentBrowser=%i", &value) == 1) {
        _showContentBrowser = static_cast<bool>(value);
    } else if (sscanf(line, "ShowPrimSpecEditor=%i", &value) == 1) {
        _showPrimSpecEditor = static_cast<bool>(value);
    } else if (sscanf(line, "ShowViewport=%i", &value) == 1) {
        _showViewport = static_cast<bool>(value);
    } else if (sscanf(line, "ShowStatusBar=%i", &value) == 1) {
        _showStatusBar = static_cast<bool>(value);
    } else if (sscanf(line, "ShowLauncherBar=%i", &value) == 1) {
        _showLauncherBar = static_cast<bool>(value);
    } else if (sscanf(line, "ShowDebugWindow=%i", &value) == 1) {
        _showDebugWindow = static_cast<bool>(value);
    } else if (sscanf(line, "ShowArrayEditor=%i", &value) == 1) {
        _showArrayEditor = static_cast<bool>(value);
    } else if (sscanf(line, "LastFileBrowserDirectory=%s", strBuffer) == 1) {
        _lastFileBrowserDirectory = strBuffer;
    } else if (strlen(line) > 12 && std::equal(line, line + 12, "RecentFiles=")) {
        std::string recentFiles(line + 12);
        _recentFiles.push_back("");
        for (auto c : recentFiles) {
            if (c == '\0')
                break;
            else if (c == ';') {
                _recentFiles.push_back("");
            } else {
                _recentFiles.back().push_back(c);
            }
        }
    } else if (sscanf(line, "MainWindowWidth=%i", &value) == 1) {
        if (value > 0) {
            _mainWindowWidth = value;
        }
    } else if (sscanf(line, "MainWindowHeight=%i", &value) == 1) {
        if (value > 0) {
            _mainWindowHeight = value;
        }
    } else if (strlen(line) > 9 && std::equal(line, line + 9, "Launcher=")) {
        std::string launcher(line + 9);
        auto semiColonPos = std::find(launcher.begin(), launcher.end(), ';');
        if (semiColonPos != launcher.end()) {
            auto pos = std::distance(launcher.begin(), semiColonPos);
            AddLauncher(launcher.substr(0, pos), launcher.substr(pos + 1));
        }
    }
}

// TODO: rewrite the function to use an internal buffer to avoid dependency on imgui
void EditorSettings::Dump(ImGuiTextBuffer *buf) {

    buf->appendf("ShowLayerHierarchyEditor=%d\n", _showLayerHierarchyEditor);
    buf->appendf("ShowLayerStackEditor=%d\n", _showLayerStackEditor);
    buf->appendf("ShowPropertyEditor=%d\n", _showPropertyEditor);
    buf->appendf("ShowOutliner=%d\n", _showOutliner);
    buf->appendf("ShowTimeline=%d\n", _showTimeline);
    buf->appendf("ShowContentBrowser=%d\n", _showContentBrowser);
    buf->appendf("ShowPrimSpecEditor=%d\n", _showPrimSpecEditor);
    buf->appendf("ShowViewport=%d\n", _showViewport);
    buf->appendf("ShowStatusBar=%d\n", _showStatusBar);
    buf->appendf("ShowLauncherBar=%d\n", _showLauncherBar);
    buf->appendf("ShowDebugWindow=%d\n", _showDebugWindow);
    buf->appendf("ShowArrayEditor=%d\n", _showArrayEditor);
    if (!_lastFileBrowserDirectory.empty()) {
        buf->appendf("LastFileBrowserDirectory=%s\n", _lastFileBrowserDirectory.c_str());
    }
    std::string recentFileString;
    for (std::list<std::string>::iterator it = _recentFiles.begin(); it != _recentFiles.end(); ++it) {
        recentFileString += *it;
        if (it != std::prev(_recentFiles.end())) {
            recentFileString.push_back(';');
        }
    }
    buf->appendf("RecentFiles=%s\n", recentFileString.c_str());
    if (_mainWindowWidth > 0) {
        buf->appendf("MainWindowWidth=%d\n", _mainWindowWidth);
    }
    if (_mainWindowHeight > 0) {
        buf->appendf("MainWindowHeight=%d\n", _mainWindowHeight);
    }
    for (int i = 0; i < _launcherNames.size(); ++i) {
        buf->appendf("Launcher=%s;%s\n", _launcherNames[i].c_str(), _launcherCommandLines[i].c_str());
    }
}

void EditorSettings::UpdateRecentFiles(const std::string &newFile) {
    auto found = find(_recentFiles.begin(), _recentFiles.end(), newFile);
    if (found != _recentFiles.end()) {
        _recentFiles.erase(found);
    }
    _recentFiles.push_front(newFile);
    if (_recentFiles.size() > 10) {
        std::list<std::string>::iterator begin = _recentFiles.begin();
        std::advance(begin, 10);
        _recentFiles.erase(begin, _recentFiles.end());
    }
}

bool EditorSettings::AddLauncher(const std::string &launcherName, const std::string &commandLine) {
    // ensure the name and command line are not empty
    if (launcherName == "" || commandLine == "")
        return false;
    // Ensure the launcher name is unique
    if (std::find(_launcherNames.begin(), _launcherNames.end(), launcherName) != _launcherNames.end())
        return false;
    // TODO check for carriage return in command line and name
    _launcherNames.emplace_back(launcherName);
    _launcherCommandLines.emplace_back(commandLine);
    return true;
}

bool EditorSettings::RemoveLauncher(const std::string &launcherName) {
    auto found = std::find(_launcherNames.begin(), _launcherNames.end(), launcherName);
    if (found == _launcherNames.end())
        return false;
    auto pos = std::distance(_launcherNames.begin(), found);
    _launcherNames.erase(_launcherNames.begin() + pos);
    _launcherCommandLines.erase(_launcherCommandLines.begin() + pos);
    return true;
}

std::string EditorSettings::GetLauncherCommandLine(const std::string &commandName) const {
    auto found = std::find(_launcherNames.begin(), _launcherNames.end(), commandName);
    if (found != _launcherNames.end()) {
        auto pos = std::distance(_launcherNames.begin(), found);
        return _launcherCommandLines[pos];
    }
    return "";
}
