#include "Constants.h"
#include "EditorSettings.h"

#include <algorithm>
#include <cstring>

#include <imgui.h> // for ImGuiTextBuffer

EditorSettings::EditorSettings() : _mainWindowWidth(InitialWindowWidth), _mainWindowHeight(InitialWindowHeight) {}

template <typename ContainerT>
inline void SplitSemiColon(const std::string &line, ContainerT &output) {
    output.push_back(""); // When we call this function we are sure there is at least one element
    for (auto c : line) {
        if (c == '\0')
            break;
        else if (c == ';') {
            output.push_back("");
        } else {
            output.back().push_back(c);
        }
    }
}

template <typename ContainerT>
inline std::string JoinSemiColon(const ContainerT &container) {
    std::string line;
    for (auto it = container.begin(); it != container.end(); ++it) {
        line += *it;
        if (it != std::prev(container.end())) {
            line.push_back(';');
        }
    }
    return line;
}


void EditorSettings::ParseLine(const char *line) {
    int value = 0;
    float valuef;
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
        _showViewport1 = static_cast<bool>(value);
    } else if (sscanf(line, "ShowViewport2=%i", &value) == 1) {
        _showViewport2 = static_cast<bool>(value);
    } else if (sscanf(line, "ShowViewport3=%i", &value) == 1) {
        _showViewport3 = static_cast<bool>(value);
    } else if (sscanf(line, "ShowViewport4=%i", &value) == 1) {
        _showViewport4 = static_cast<bool>(value);
    } else if (sscanf(line, "ShowStatusBar=%i", &value) == 1) {
        _showStatusBar = static_cast<bool>(value);
    } else if (sscanf(line, "ShowDebugWindow=%i", &value) == 1) {
        _showDebugWindow = static_cast<bool>(value);
    } else if (sscanf(line, "ShowArrayEditor=%i", &value) == 1) {
        _showSdfAttributeEditor = static_cast<bool>(value);
    } else if (sscanf(line, "ShowHydraBrowser=%i", &value) == 1) {
        _showHydraBrowser = static_cast<bool>(value);
    } else if (sscanf(line, "ShowHydraNoticeLogger=%i", &value) == 1) {
        _showHydraNoticeLogger = static_cast<bool>(value);
    } else if (strlen(line) > 6 && std::equal(line, line + 6, "Addon.")) {
        // Addon.<id>.<key>=<value>
        const char *eq = strchr(line, '=');
        if (eq) {
            std::string key(line + 6, eq - (line + 6));
            std::string val(eq + 1);
            // Trim trailing newline if present.
            if (!val.empty() && val.back() == '\n') val.pop_back();
            _addonValues[key] = val;
        }
    } else if (sscanf(line, "ShowValidator=%i", &value) == 1) {
        _showValidator = static_cast<bool>(value);
    } else if (sscanf(line, "ShowConnectionEditor=%i", &value) == 1) {
        _showUsdConnectionEditor = static_cast<bool>(value);
    } else if (sscanf(line, "ShowSearch=%i", &value) == 1) {
        _showSearch = static_cast<bool>(value);
    } else if (sscanf(line, "ShowSplashScreen=%i", &value) == 1) {
        _showSplashScreen = static_cast<bool>(value);
    } else if (sscanf(line, "LastFileBrowserDirectory=%s", strBuffer) == 1) {
        _lastFileBrowserDirectory = strBuffer;
    } else if (strlen(line) > 12 && std::equal(line, line + 12, "RecentFiles=")) {
        std::string recentFilesLine(line + 12);
        SplitSemiColon(recentFilesLine, _recentFiles);
    } else if (sscanf(line, "MainWindowWidth=%i", &value) == 1) {
        if (value > 0) {
            _mainWindowWidth = value;
        }
    } else if (sscanf(line, "MainWindowHeight=%i", &value) == 1) {
        if (value > 0) {
            _mainWindowHeight = value;
        }
    } else if (strlen(line) > 12 && std::equal(line, line + 12, "PluginPaths=")) {
        std::string pluginPathsLine(line + 12);
        SplitSemiColon(pluginPathsLine, _pluginPaths);
    } else if (strlen(line) > 19 && std::equal(line, line + 19, "BlueprintLocations=")) {
        std::string blueprintsLine(line + 19);
        SplitSemiColon(blueprintsLine, _blueprintLocations);
    } else if (sscanf(line, "UiScale=%f", &valuef) == 1) {
        _uiScale = valuef;
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
    buf->appendf("ShowViewport=%d\n", _showViewport1);
    buf->appendf("ShowViewport2=%d\n", _showViewport2);
    buf->appendf("ShowViewport3=%d\n", _showViewport3);
    buf->appendf("ShowViewport4=%d\n", _showViewport4);
    buf->appendf("ShowStatusBar=%d\n", _showStatusBar);
    buf->appendf("ShowDebugWindow=%d\n", _showDebugWindow);
    buf->appendf("ShowArrayEditor=%d\n", _showSdfAttributeEditor);
    buf->appendf("ShowHydraBrowser=%d\n", _showHydraBrowser);
    buf->appendf("ShowHydraNoticeLogger=%d\n", _showHydraNoticeLogger);
    buf->appendf("ShowValidator=%d\n", _showValidator);
    buf->appendf("ShowConnectionEditor=%d\n", _showUsdConnectionEditor);
    buf->appendf("ShowSearch=%d\n", _showSearch);
    buf->appendf("ShowSplashScreen=%d\n", _showSplashScreen);
    if (!_lastFileBrowserDirectory.empty()) {
        buf->appendf("LastFileBrowserDirectory=%s\n", _lastFileBrowserDirectory.c_str());
    }
    if (!_recentFiles.empty()) {
        buf->appendf("RecentFiles=%s\n", JoinSemiColon(_recentFiles).c_str());
    }
    if (_mainWindowWidth > 0) {
        buf->appendf("MainWindowWidth=%d\n", _mainWindowWidth);
    }
    if (_mainWindowHeight > 0) {
        buf->appendf("MainWindowHeight=%d\n", _mainWindowHeight);
    }
    if (!_pluginPaths.empty()) {
        buf->appendf("PluginPaths=%s\n", JoinSemiColon(_pluginPaths).c_str());
    }
    if (!_blueprintLocations.empty()) {
        buf->appendf("BlueprintLocations=%s\n", JoinSemiColon(_blueprintLocations).c_str());
    }
    buf->appendf("UiScale=%f\n", _uiScale);
    for (const auto &kv : _addonValues) {
        buf->appendf("Addon.%s=%s\n", kv.first.c_str(), kv.second.c_str());
    }
}

bool EditorSettings::GetAddonBool(const std::string &addonId, const std::string &key, bool defaultValue) const {
    auto it = _addonValues.find(addonId + "." + key);
    if (it == _addonValues.end()) return defaultValue;
    return it->second != "0";
}

void EditorSettings::SetAddonBool(const std::string &addonId, const std::string &key, bool value) {
    _addonValues[addonId + "." + key] = value ? "1" : "0";
}

std::string EditorSettings::GetAddonString(const std::string &addonId, const std::string &key,
                                           const std::string &defaultValue) const {
    auto it = _addonValues.find(addonId + "." + key);
    return it == _addonValues.end() ? defaultValue : it->second;
}

void EditorSettings::SetAddonString(const std::string &addonId, const std::string &key, const std::string &value) {
    _addonValues[addonId + "." + key] = value;
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

